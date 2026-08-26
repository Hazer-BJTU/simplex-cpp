#pragma once

//
// model_io.hpp — Model I/O data contract
// =======================================
//
// Data structures describing a conversation with an LLM model and the tool
// invocations (ReAct) it performs. These types are the shared contract the
// project roadmap reserves for the future "model adapter" and "agentloop"
// modules. Every record is a plain aggregate (pure composition, no base
// classes) serialised through the nlohmann JSON ADL mechanism: a pair of free
// functions to_json(json&, const T&) / from_json(const json&, T&) sits next
// to each struct in this namespace, which makes
//
//     nlohmann::json j = record;   // to_json
//     auto copy = j.get<T>();      // from_json (T must be default-constructible)
//     j.get_to(record);            // from_json in place
//
// work with no inheritance and no boilerplate. The ADL pair MUST be declared
// before any type that embeds the record by value, so each struct is followed
// immediately by its to_json/from_json and the types are ordered so
// dependencies come first: Content -> InvokeQuery -> InvokeReturn ->
// TokenCost -> MessageItem -> AgentLoopStep -> UserLoopStep -> Invocable ->
// MetaInfo.
// PromptTemplate (prompt_template.hpp) carries its own pair under this same
// protocol, which is what lets the session container AgentInputState close
// the file WITH one: the whole session — the STRUCTURED system prompt
// included, sections not rendered markdown — round-trips as JSON, while
// render() stays the in-memory assembly into the text a provider request
// finally embeds.
//
// The tool-definition record is part of the contract: Invocable (tool
// registration section) names a tool and carries its argument schema; the
// per-call type/security metadata rides in InvokeQuery. The session-level
// container is AgentInputState (end of file): system prompt + registered
// tools + turns — the complete input a (future) interpreter consumes to
// build one provider request.
//
// Overall model-I/O management logic
// ----------------------------------
// The conversation is organised as a two-tier loop, held together at the top
// by the session container:
//
//   AgentInputState                          (session input; end of file)
//   +-- system_prompt : PromptTemplate        the system prompt (markdown)
//   +-- tools         : vector<Invocable>     tools registered this session
//   +-- turns         : vector<UserLoopStep>  <- one entry per user turn
//
//   UserLoopStep
//   +-- user_input      : MessageItem          (one user message)
//   +-- agent_loop_step : vector<AgentLoopStep> <- the "agent loop"
//
//   AgentLoopStep models one ReAct cycle inside the agent loop:
//     model_response  — an assistant message (text / reasoning / tool calls)
//     invoke_returns  — the tool results produced by the calls in that
//                       response; each item may embed the InvokeReturn record
//                       whose query.id names the call it answers
//
//   - User loop:  one user turn, possibly followed by several agent
//                 reasoning/acting steps until the model emits a final answer.
//   - Agent loop: the ReAct cycle — the model emits a response (optionally with
//                 tool calls `invokes`), tools execute, and their outputs come
//                 back as `invoke_returns`.
//
// Context management under a token budget:
//   - AgentLoopStep::retain_priority — trim preference. When the context window
//                 must shed tokens, agent steps are dropped low->high priority;
//                 `Pinned` steps are never auto-trimmed.
//   - UserLoopStep::retain_priority  — compact-selection preference. When turns
//                 are chosen for compaction (summarisation) this biases the
//                 selection; `Pinned` turns are kept verbatim.
//
// Every composite struct carries an `extras` (std::optional<nlohmann::json>)
// escape hatch so the contract can carry provider-specific or experimental
// fields without a schema change.
//
// ----------------------------------------------------------------------------
// SERIALIZATION PROTOCOL
// ----------------------------------------------------------------------------
//  1. Each record serialises to a JSON object.
//  2. Object keys are snake_case and match the C++ field names.
//  3. Optional fields are OMITTED when empty and present when set (never
//     emitted as JSON null); a missing key on read yields std::nullopt.
//  4. Enumerations serialise as lowercase snake_case STRING names
//     (e.g. ContentType::ExternalRef -> "external_ref"), never integers. An
//     unrecognised value on read falls back to the first listed mapping, so
//     types should list their safest/neutral value first.
//  5. nlohmann::json fields (e.g. arguments, extras) embed inline as-is.
//  6. Unknown keys are ignored on read (forward-compatible). A MISSING key
//     keeps the member's default (plain fields) or yields std::nullopt
//     (optional fields) — members therefore carry meaningful default
//     initialisers, and from_json reads through find() guards instead of
//     at(), which would throw.
//  7. Round-trip invariant: json(x).get<X>() reproduces x.
//

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "dataclass/prompt_template.hpp"

namespace model_io {

// ---- null-tolerant optional reads ---------------------------------------------
//
// Protocol rules 3+6 hardened against laxer external producers: a JSON null
// under an optional key reads as absent (std::nullopt), exactly like a
// missing key — never a type error (which would fail the whole session load)
// and never an engaged optional holding a default-constructed value.
namespace detail {
template<typename T>
void read_optional(const nlohmann::json& j, const char* key,
                   std::optional<T>& member) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) {
        member = it->get<T>();
    } else {
        member.reset();
    }
}
} // namespace detail

// ---- conversation data types ------------------------------------------------

// How the bytes of a Content payload are encoded in `raw`.
enum class ContentType {
    Text,        // `raw` is UTF-8 text.
    Binary,      // `raw` is base64-encoded binary.
    ExternalRef, // `raw` is a URI / pointer to out-of-band content.
};

NLOHMANN_JSON_SERIALIZE_ENUM(ContentType, {
    {ContentType::Text, "text"},
    {ContentType::Binary, "binary"},
    {ContentType::ExternalRef, "external_ref"},
})

// A single piece of user/model content.
struct Content {
    ContentType type = ContentType::Text;
    std::string raw;
    // Provider-specific content-part fields beyond what ContentType can say —
    // e.g. the Responses API's part "type" ("output_text", ...) alongside our
    // coarse text/binary/external_ref, or annotations. Consumers that map to
    // a plain string content ignore it.
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const Content& c) {
    j = nlohmann::json{{"type", c.type}, {"raw", c.raw}};
    if (c.extras) j["extras"] = *c.extras;
}

inline void from_json(const nlohmann::json& j, Content& c) {
    if (auto it = j.find("type"); it != j.end()) it->get_to(c.type);
    if (auto it = j.find("raw"); it != j.end()) it->get_to(c.raw);
    detail::read_optional(j, "extras", c.extras);
}

// How a tool invocation may touch state.
enum class InvokeType {
    ReadOnly,    // no side effects; safe to run in parallel.
    ParallWrite, // writes state, may be parallelised with other writers.
    SerialWrite, // writes state; must run serially.
};

NLOHMANN_JSON_SERIALIZE_ENUM(InvokeType, {
    {InvokeType::ReadOnly, "read_only"},
    {InvokeType::ParallWrite, "parall_write"},
    {InvokeType::SerialWrite, "serial_write"},
})

// Trust level governing whether a tool invocation runs automatically.
enum class InvokeSecurity {
    DefaultDeny,    // refuse unless explicitly allowed.
    RequireConfirm, // ask the user before running.
    Trusted,        // run without prompting.
};

// DefaultDeny first: an unrecognised security level fails closed.
NLOHMANN_JSON_SERIALIZE_ENUM(InvokeSecurity, {
    {InvokeSecurity::DefaultDeny, "default_deny"},
    {InvokeSecurity::RequireConfirm, "require_confirm"},
    {InvokeSecurity::Trusted, "trusted"},
})

// A request to invoke a tool (a "tool call").
struct InvokeQuery {
    InvokeType type = InvokeType::ReadOnly;
    InvokeSecurity security = InvokeSecurity::DefaultDeny;
    std::string id, name;
    nlohmann::json arguments = nlohmann::json::object();
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const InvokeQuery& q) {
    j = nlohmann::json{
        {"type", q.type},
        {"security", q.security},
        {"id", q.id},
        {"name", q.name},
        {"arguments", q.arguments},
    };
    if (q.extras) j["extras"] = *q.extras;
}

inline void from_json(const nlohmann::json& j, InvokeQuery& q) {
    if (auto it = j.find("type"); it != j.end()) it->get_to(q.type);
    if (auto it = j.find("security"); it != j.end()) it->get_to(q.security);
    if (auto it = j.find("id"); it != j.end()) it->get_to(q.id);
    if (auto it = j.find("name"); it != j.end()) it->get_to(q.name);
    if (auto it = j.find("arguments"); it != j.end()) it->get_to(q.arguments);
    detail::read_optional(j, "extras", q.extras);
}

// The result of executing an InvokeQuery.
struct InvokeReturn {
    InvokeQuery query;
    Content output;
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const InvokeReturn& r) {
    j = nlohmann::json{{"query", r.query}, {"output", r.output}};
    if (r.extras) j["extras"] = *r.extras;
}

inline void from_json(const nlohmann::json& j, InvokeReturn& r) {
    if (auto it = j.find("query"); it != j.end()) it->get_to(r.query);
    if (auto it = j.find("output"); it != j.end()) it->get_to(r.output);
    detail::read_optional(j, "extras", r.extras);
}

// Token accounting for one model exchange, as reported by the provider's
// usage block (e.g. the Responses API's `usage`: input_tokens /
// output_tokens / input_tokens_details.cached_tokens). Plain integer counts;
// `cache_hit` counts the prompt tokens served from the provider's cache and
// is a SUBSET of `prompt`, not additive to it. Carried on
// MessageItem::cost, which only ModelResponse items populate.
struct TokenCost {
    std::uint64_t prompt = 0;    // tokens the request fed the model.
    std::uint64_t generated = 0; // tokens the model produced.
    std::uint64_t cache_hit = 0; // prompt tokens served from cache (subset).
};

inline void to_json(nlohmann::json& j, const TokenCost& c) {
    j = nlohmann::json{
        {"prompt", c.prompt},
        {"generated", c.generated},
        {"cache_hit", c.cache_hit},
    };
}

inline void from_json(const nlohmann::json& j, TokenCost& c) {
    if (auto it = j.find("prompt"); it != j.end()) it->get_to(c.prompt);
    if (auto it = j.find("generated"); it != j.end())
        it->get_to(c.generated);
    if (auto it = j.find("cache_hit"); it != j.end())
        it->get_to(c.cache_hit);
}

// What kind of conversational item a MessageItem represents.
enum class MessageItemType {
    UserInput,     // a user message (`role` typically "user").
    ModelResponse, // a model/assistant message (`role` typically "assistant").
    InvokeReturn,  // a tool result; the value rides in `content`.
};

NLOHMANN_JSON_SERIALIZE_ENUM(MessageItemType, {
    {MessageItemType::UserInput, "user_input"},
    {MessageItemType::ModelResponse, "model_response"},
    {MessageItemType::InvokeReturn, "invoke_return"},
})

// A single message. Overloaded across the three MessageItemType roles via
// `type`; the optional fields are populated as the role demands (e.g.
// `reasoning`/`invokes`/`cost` on model responses, the result in `content`
// for invoke returns). An invoke-return item may additionally embed its
// originating InvokeReturn record in `invoke_return`: the query inside
// carries the id that correlates the result back to the entry in the
// model_response's `invokes` (the wire-level "tool call id" providers
// require on tool-result messages). `content` is an ordered list so one
// message can carry heterogeneous parts (for example text followed by an
// image). For invoke returns, the record's `output` is expected to match the
// first content part when both are set.
struct MessageItem {
    MessageItemType type = MessageItemType::UserInput;
    std::string role;
    std::vector<Content> content;
    std::optional<Content> reasoning, action_status;
    std::optional<std::vector<InvokeQuery>> invokes;
    // On a ModelResponse item: the exchange's token accounting as the
    // provider reported it (nullopt when the provider reported none).
    std::optional<TokenCost> cost;
    // On an InvokeReturn item: the originating call + result as one record —
    // provenance for correlating the result to the call that produced it.
    std::optional<InvokeReturn> invoke_return;
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const MessageItem& m) {
    j = nlohmann::json{
        {"type", m.type},
        {"role", m.role},
        {"content", m.content},
    };
    if (m.reasoning) j["reasoning"] = *m.reasoning;
    if (m.action_status) j["action_status"] = *m.action_status;
    if (m.invokes) j["invokes"] = *m.invokes;
    if (m.cost) j["cost"] = *m.cost;
    if (m.invoke_return) j["invoke_return"] = *m.invoke_return;
    if (m.extras) j["extras"] = *m.extras;
}

inline void from_json(const nlohmann::json& j, MessageItem& m) {
    if (auto it = j.find("type"); it != j.end()) it->get_to(m.type);
    if (auto it = j.find("role"); it != j.end()) it->get_to(m.role);
    if (auto it = j.find("content"); it != j.end()) {
        // Read the former single-object representation as a one-element list
        // so persisted conversations remain loadable after the protocol
        // migration. New writes always use the array representation.
        if (it->is_array()) {
            it->get_to(m.content);
        } else if (!it->is_null()) {
            m.content = {it->get<Content>()};
        } else {
            m.content.clear();
        }
    }
    detail::read_optional(j, "reasoning", m.reasoning);
    detail::read_optional(j, "action_status", m.action_status);
    detail::read_optional(j, "invokes", m.invokes);
    detail::read_optional(j, "cost", m.cost);
    detail::read_optional(j, "invoke_return", m.invoke_return);
    detail::read_optional(j, "extras", m.extras);
}

// Retention preference shared by agent-loop and user-loop steps (see file
// header). Higher value = kept longer / harder to drop. Serialised as a
// lowercase string ("discardable" / "normal" / "pinned").
enum class RetainPriority {
    Normal,      // default policy.
    Discardable, // may be dropped/summarised first under context pressure.
    Pinned,      // never auto-trimmed (agent step) / kept verbatim (user turn).
};

// Normal first: an unrecognised priority defaults to the neutral policy.
NLOHMANN_JSON_SERIALIZE_ENUM(RetainPriority, {
    {RetainPriority::Normal, "normal"},
    {RetainPriority::Discardable, "discardable"},
    {RetainPriority::Pinned, "pinned"},
})

// One agent-loop step: a model response plus the tool results its calls
// produced (a single ReAct cycle).
struct AgentLoopStep {
    MessageItem model_response;
    std::optional<std::vector<MessageItem>> invoke_returns;
    std::optional<nlohmann::json> extras;
    // Trim preference — order in which this step is dropped to fit the budget.
    RetainPriority retain_priority = RetainPriority::Normal;
};

inline void to_json(nlohmann::json& j, const AgentLoopStep& s) {
    j = nlohmann::json{
        {"model_response", s.model_response},
        {"retain_priority", s.retain_priority},
    };
    if (s.invoke_returns) j["invoke_returns"] = *s.invoke_returns;
    if (s.extras) j["extras"] = *s.extras;
}

inline void from_json(const nlohmann::json& j, AgentLoopStep& s) {
    if (auto it = j.find("model_response"); it != j.end())
        it->get_to(s.model_response);
    detail::read_optional(j, "invoke_returns", s.invoke_returns);
    detail::read_optional(j, "extras", s.extras);
    if (auto it = j.find("retain_priority"); it != j.end())
        it->get_to(s.retain_priority);
}

// One user-loop step: a user message plus the agent steps it triggered.
struct UserLoopStep {
    MessageItem user_input;
    std::vector<AgentLoopStep> agent_loop_step;
    std::optional<nlohmann::json> extras;
    // Compact-selection preference — bias for summarising vs. keeping this turn.
    RetainPriority retain_priority = RetainPriority::Normal;
};

inline void to_json(nlohmann::json& j, const UserLoopStep& u) {
    j = nlohmann::json{
        {"user_input", u.user_input},
        {"agent_loop_step", u.agent_loop_step},
        {"retain_priority", u.retain_priority},
    };
    if (u.extras) j["extras"] = *u.extras;
}

inline void from_json(const nlohmann::json& j, UserLoopStep& u) {
    if (auto it = j.find("user_input"); it != j.end())
        it->get_to(u.user_input);
    if (auto it = j.find("agent_loop_step"); it != j.end())
        it->get_to(u.agent_loop_step);
    detail::read_optional(j, "extras", u.extras);
    if (auto it = j.find("retain_priority"); it != j.end())
        it->get_to(u.retain_priority);
}

// ---- tool registration -------------------------------------------------------

// A tool exposed to the model: what it is called, what it does, and the
// JSON-Schema contract its arguments must satisfy. Registering an Invocable
// makes the tool available; the model then calls it through InvokeQuery,
// which carries the per-call type/security metadata and the arguments
// themselves.
struct Invocable {
    std::string name, description;
    // JSON Schema the `arguments` of an InvokeQuery must satisfy — the
    // `parameters` / `input_schema` object tool definitions carry on the
    // wire. The default `{}` is a schema that accepts anything, including
    // no arguments at all (null is not a valid JSON Schema).
    nlohmann::json argument_schema = nlohmann::json::object();
    // Kind of remote/plugin-registered tool, for provider-specific dispatch;
    // nullopt for plain in-process tools.
    std::optional<std::string> remote_type;
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const Invocable& v) {
    j = nlohmann::json{
        {"name", v.name},
        {"description", v.description},
        {"argument_schema", v.argument_schema},
    };
    if (v.remote_type) j["remote_type"] = *v.remote_type;
    if (v.extras) j["extras"] = *v.extras;
}

inline void from_json(const nlohmann::json& j, Invocable& v) {
    if (auto it = j.find("name"); it != j.end()) it->get_to(v.name);
    if (auto it = j.find("description"); it != j.end()) it->get_to(v.description);
    if (auto it = j.find("argument_schema"); it != j.end()) v.argument_schema = *it;
    detail::read_optional(j, "remote_type", v.remote_type);
    detail::read_optional(j, "extras", v.extras);
}

// ---- session bookkeeping --------------------------------------------------------

// Session lifecycle as the HOST sees it. The model never sees any of this:
// interpreters never map MetaInfo into provider requests.
enum class SessionStatus {
    Active,    // live; no terminal outcome yet (default, and the fallback
               // for an unrecognised status string on read).
    Completed, // ended normally.
    Failed,    // terminated by an unrecoverable error; MetaInfo::error
               // carries the record.
};

NLOHMANN_JSON_SERIALIZE_ENUM(SessionStatus, {
    {SessionStatus::Active, "active"},
    {SessionStatus::Completed, "completed"},
    {SessionStatus::Failed, "failed"},
})

// System-written session bookkeeping — identity and outcome, not
// conversation. Written by the HOST only: the user and the model never
// write it, plugins included. created_at / updated_at are REQUIRED in the
// persistence sense — to_json always emits them, the host mints created_at
// at session start and refreshes updated_at on every write; an empty
// string on read flags a misbehaving host, not an absent value.
struct MetaInfo {
    // Format version of THIS record — so a restored session can be told
    // apart from an older shape. Bump when the fields below change
    // meaning; readers stay lenient (unknown keys ignored).
    std::uint32_t schema_version = 1;
    // Host-minted stable identity of the session (e.g. a uuid); "" until
    // the host mints one.
    std::string session_id;
    // ISO-8601 (RFC 3339) timestamp of session creation; host-maintained.
    std::string created_at;
    // ISO-8601 (RFC 3339) timestamp of the last write; host-maintained.
    std::string updated_at;
    // Lifecycle status; Active until the session ends one way or another.
    SessionStatus status = SessionStatus::Active;
    // The unrecoverable error that ended the session, as a free-form
    // object (conventionally {"kind", "message", "at"}); absent while the
    // session is healthy. When present, status is Failed.
    std::optional<nlohmann::json> error;
};

inline void to_json(nlohmann::json& j, const MetaInfo& m) {
    j = nlohmann::json{
        {"schema_version", m.schema_version},
        {"session_id", m.session_id},
        {"created_at", m.created_at},
        {"updated_at", m.updated_at},
        {"status", m.status},
    };
    if (m.error) j["error"] = *m.error;
}

inline void from_json(const nlohmann::json& j, MetaInfo& m) {
    if (auto it = j.find("schema_version"); it != j.end())
        it->get_to(m.schema_version);
    if (auto it = j.find("session_id"); it != j.end())
        it->get_to(m.session_id);
    if (auto it = j.find("created_at"); it != j.end())
        it->get_to(m.created_at);
    if (auto it = j.find("updated_at"); it != j.end())
        it->get_to(m.updated_at);
    if (auto it = j.find("status"); it != j.end()) it->get_to(m.status);
    detail::read_optional(j, "error", m.error);
}

// ---- session input container ---------------------------------------------------

// AgentInputState — everything one model invocation needs
// ========================================================
//
// The session-level container the file header reserved: the complete input a
// (future) interpreter interface turns into ONE provider request. The host
// (agent runtime) owns an AgentInputState for the session, evolves it as the
// session runs (append prompt regions, register tools, push turns), and hands
// it to the interpreter before each model call; the interpreter maps it
// directly onto a request the way endpoint/example/deepseek_chat.cpp's
// build_request() assembles a {model, stream, messages[], tools[]}
// chat-completion body.
//
// Scope — the CONVERSATION only. Model and generation settings (model name,
// base_url, api key, temperature, stream, max_tokens, ...) are deliberately
// NOT part of AgentInputState: they are provider/endpoint configuration the
// interpreter owns separately and merges into the request it builds — in the
// deepseek example, model_name(), the parsed base_url and the api_key come
// from the caller's configuration, not from the conversation. This type
// changes when the conversation changes, not when the deployment does.
//
// Structure, recursed to the leaves:
//
//   AgentInputState
//   |
//   +-- meta : MetaInfo
//   |      Host-written session bookkeeping: identity (session_id),
//   |      REQUIRED timestamps (created_at / updated_at), the record's
//   |      schema_version, the lifecycle status, and the terminal error
//   |      record when the session cannot continue. Never mapped into
//   |      provider requests — the model never sees it.
//   |
//   +-- system_prompt : PromptTemplate
//   |      The system prompt as managed markdown (prompt_template.hpp —
//   |      structured storage with cache-stable tiering; serialises as its
//   |      ordered sections under this file's protocol). Laid
//   |      out for byte-prefix provider caches: immutable regions first,
//   |      growing regions next, volatile regions last; the class enforces
//   |      both the order and the per-tier mutation rules.
//   |      +-- heading_level : int
//   |      |    '#' depth of every section title; default 2, validated
//   |      |    1..6 at render().
//   |      +-- sections (read via begin()/end()/find(name))
//   |      |    one PromptSection per add_section(), in order:
//   |      |      name      : string  unique key ("tools")
//   |      |      title     : string  heading text; "" renders body only
//   |      |      stability : SectionStability
//   |      |                  Immutable — fixed at creation; no mutation path
//   |      |                  Growing   — append() only; bytes only grow
//   |      |                  Volatile  — rewrite()/erase(); lives at tail
//   |      |      text      : string  canonicalised body; emitted verbatim
//   |      +-- render() -> RenderedPrompt   (pure, deterministic)
//   |           markdown : string           the assembled system prompt
//   |           spans    : vector<SectionSpan>
//   |                     one [begin, end) byte range per rendered section,
//   |                     carrying its name and stability — for cache-prefix
//   |                     accounting
//   |
//   +-- tools : vector<Invocable>
//   |      The tools registered for the session — what the model may call.
//   |      Each Invocable:
//   |        name            : string  wire name the model calls
//   |        description     : string  what the tool does
//   |        argument_schema : json    JSON Schema an InvokeQuery's
//   |                                `arguments` must satisfy
//   |        remote_type?    : optional<string>  plugin/remote dispatch kind;
//   |                                            nullopt = in-process tool
//   |        extras?         : optional<json>    provider-specific metadata
//   |
//   +-- turns : vector<UserLoopStep>
//   |      The conversation so far, oldest user turn first. Each UserLoopStep
//   |      is one user turn:
//   |        user_input      : MessageItem  the user message that started it
//   |        agent_loop_step : vector<AgentLoopStep>  the ReAct cycles it
//   |                          triggered, in order; each AgentLoopStep is one
//   |                          cycle (see the struct above):
//   |                            model_response  : MessageItem  the assistant
//   |                                  message — text / reasoning / invokes
//   |                            invoke_returns? : vector<MessageItem>  the
//   |                                  tool results the calls in that
//   |                                  response produced (payload in content)
//   |                            retain_priority : RetainPriority  trim
//   |                                  preference under the token budget
//   |                            extras?         : optional<json>
//   |        retain_priority : RetainPriority  Normal/Discardable/Pinned;
//   |                          biases turn compaction (see file header)
//   |        extras?         : optional<json>
//   |      recursing each MessageItem (user_input, every model_response, and
//   |      every invoke_returns entry):
//   |        type           : MessageItemType  user_input | model_response |
//   |                                           invoke_return
//   |        role           : string   "user" / "assistant" / "tool"
//   |        content        : vector<Content>  ordered, heterogeneous parts;
//   |                          each part has type : ContentType (text | binary |
//   |                               external_ref) — how `raw` is encoded;
//   |                               raw  : string payload;
//   |                               extras? : optional<json> content-part
//   |                               fields beyond ContentType (e.g. the
//   |                               Responses API part "type")
//   |        reasoning?     : optional<Content>  chain-of-thought, if any
//   |        action_status? : optional<Content>  lifecycle annotation
//   |        invokes?       : optional<vector<InvokeQuery>> — the tool calls
//   |                          a model response made. Each InvokeQuery:
//   |                            type     : InvokeType     read_only |
//   |                                      parall_write | serial_write
//   |                            security : InvokeSecurity default_deny |
//   |                                      require_confirm | trusted
//   |                            id, name : call id; `name` must resolve to
//   |                                      an Invocable in `tools`
//   |                            arguments: json satisfying that Invocable's
//   |                                      argument_schema
//   |                            extras?  : optional<json>
//   |        cost?          : optional<TokenCost> — on model_response items,
//   |                          the exchange's token counts (prompt /
//   |                          generated / cache_hit) as the provider
//   |                          reported them
//   |        invoke_return? : optional<InvokeReturn> — on an invoke_return
//   |                          item, the originating call + result as one
//   |                          record; query.id correlates the result back to
//   |                          the invokes entry of the model_response that
//   |                          made the call (the wire "tool call id"). The
//   |                          record recurses as the InvokeReturn struct
//   |                          above: query / output / extras?.
//   |        extras?        : optional<json>
//   |
//   +-- extras : optional<json>
//          Escape hatch for the session state itself (provider-specific or
//          experimental knobs the interpreter should pass through) —
//          EXCEPT two reserved regions: "external_status" (per-writer
//          state slots hooks/plugins sync for persistence) and "events"
//          (the append-only broadcast they publish to). The serial event
//          exchange those regions form is the ONE persistence entry/exit
//          for external information, and it never reaches a provider
//          request; mutation of AgentInputState is strictly serialised, so
//          positions in "events" carry that order (see the external
//          exchange section at the end of this file).
//
// Serialisation: the full ADL pair below — the STRUCTURED form is the
// durable one. system_prompt serialises as its ordered sections
// (prompt_template.hpp, same protocol), never as rendered markdown:
// rendering is the in-memory assembly this container defers to
// request-build time, so a restored session renders the identical bytes
// again. json(state).get<AgentInputState>() reproduces the session whole.
//
// Interpreter consumption (why this type exists): before each model call the
// interpreter flattens an AgentInputState straight into a provider request
// (cf. endpoint/example/deepseek_chat.cpp::build_request):
//   - system_prompt.render().markdown -> messages[0] (the system message);
//     its spans mark where the immutable/growing prefix ends, so an adapter
//     can place provider cache breakpoints and keep the volatile tail last
//   - tools -> the request's tool definitions; name + description +
//     argument_schema is exactly the wire triple providers expect
//   - turns -> the message list, flattened: each user_input becomes a user
//     message; each AgentLoopStep's model_response becomes an assistant
//     message (its invokes -> tool_calls) and its invoke_returns become
//     tool-result messages, correlated to their calls by the embedded
//     invoke_return's query.id (the wire "tool_call_id" providers require)
//   - extras -> interpreter/provider-specific request parameters, MINUS
//     the reserved "external_status" and "events" regions: those are
//     host-side exchange for hooks/plugins, never model context (the same
//     exemption meta enjoys)
//   Everything else a request needs — model name, base_url, temperature,
//   stream, max_tokens, ... — comes from the interpreter's provider/endpoint
//   configuration, NOT from this struct (see Scope above).
// The mapping lives in the interpreter, never here: this struct is pure data.
struct AgentInputState {
    // Host-written session bookkeeping: identity, timestamps, format
    // version, lifecycle status, terminal error — never part of what the
    // model sees.
    MetaInfo meta;
    // The system prompt: managed markdown, rendered by the interpreter at
    // request-build time (serialises as its structured sections, never as
    // rendered markdown — the structure is the reusable artefact).
    PromptTemplate system_prompt;
    // Tools registered for this session — what the model may invoke.
    std::vector<Invocable> tools;
    // The conversation so far, oldest user turn first.
    std::vector<UserLoopStep> turns;
    // Escape hatch: provider-specific / experimental session-level fields —
    // except the reserved "external_status" and "events" regions (see the
    // external exchange section at the end of this file).
    std::optional<nlohmann::json> extras;
};

inline void to_json(nlohmann::json& j, const AgentInputState& s) {
    j = nlohmann::json{
        {"meta", s.meta},
        {"system_prompt", s.system_prompt},
        {"tools", s.tools},
        {"turns", s.turns},
    };
    if (s.extras) j["extras"] = *s.extras;
}

inline void from_json(const nlohmann::json& j, AgentInputState& s) {
    if (auto it = j.find("meta"); it != j.end()) it->get_to(s.meta);
    if (auto it = j.find("system_prompt"); it != j.end())
        it->get_to(s.system_prompt);
    if (auto it = j.find("tools"); it != j.end()) it->get_to(s.tools);
    if (auto it = j.find("turns"); it != j.end()) it->get_to(s.turns);
    detail::read_optional(j, "extras", s.extras);
}

// ---- external exchange regions (external_status + events) ------------------------
//
// Two fixed regions inside AgentInputState::extras where loop hooks and
// plugins exchange host-side information with the serial agent loop — the
// ONE entry/exit through which external (non-contract) information becomes
// persistent, because both regions ride extras' serialisation:
//
//   extras["external_status"]  an object keyed by writer identity — each
//                              writer's OWN state slot, OVERWRITTEN on
//                              sync. The durable "where I left off" a
//                              plugin restores after a session round-trip.
//   extras["events"]           an append-only array of HookEvent records —
//                              broadcast messages, in the order they were
//                              published.
//
// Region protocol — what keeps parallel writers from crosstalk:
//
//   1. OWNERSHIP. external_status is written ONLY through
//      sync_external_status, events ONLY through publish_hook_event, both
//      by hooks/plugins (and the host). Everything else in extras stays
//      interpreter pass-through parameters: hooks never write outside the
//      two regions, and nobody else writes inside them.
//   2. IDENTITY ON EVERY WRITE. Every write names a non-empty `source`
//      (the writer's registration identity — a plugin id, a hook name);
//      the APIs reject empty ones with std::logic_error. In
//      external_status the source IS the slot: a writer's permit covers
//      exactly its own slot and nothing else. In events the source rides
//      on every record, so the history stays attributable.
//   3. DIFFERENT SEMANTICS PER REGION. external_status is per-source
//      LATEST state: sync overwrites the slot, no history kept. events is
//      append-only broadcast: no edit or remove API exists (deliberately)
//      — a changed state is a NEW event; readers take the latest match
//      (latest_hook_event), so per-key semantics are last-writer-wins
//      without ever rewriting history. Publish to events when others must
//      SEE something happened; sync to external_status when the writer
//      itself must REMEMBER where it stood.
//   4. STRICTLY SERIAL MUTATION, ORDER DEPENDS ON IT. Every mutation of
//      AgentInputState — appending turns, refreshing meta, syncing
//      external_status, publishing events — is strictly serialised by the
//      host: ONE total order, no concurrent writers, no locks (none are
//      needed). Order is therefore MEANINGFUL: an event's position in
//      `events` is its exact place among all session mutations, and an
//      external_status slot reflects the last sync in that same order —
//      reading both regions together yields a coherent history. Multi-
//      threaded mutation is out of contract.
//   5. PRUNING BELONGS TO THE HOST. The host may shed old entries from
//      `events` under a budget; readers must tolerate their older events
//      disappearing. external_status slots are never shed — dropping a
//      writer's latest state would defeat its persistence.
//   6. THE WIRE NEVER SEES EITHER REGION. Interpreters map extras into
//      provider requests MINUS external_status and events: the regions
//      are host-side exchange, never model context (the same exemption
//      meta enjoys).

// The reserved regions' keys inside AgentInputState::extras.
inline constexpr char kExternalStatusKey[] = "external_status";
inline constexpr char kEventsKey[] = "events";

// One broadcast record: an attributed, named, timestamped payload. Plain
// data.
struct HookEvent {
    std::string source; // the writer's identity (registration name / plugin
                        // id); required, enforced by publish_hook_event.
    std::string key;    // the event name, e.g. "model_switched"; required.
    std::string at;     // ISO-8601 (RFC 3339) time of writing.
    nlohmann::json detail = nlohmann::json::object(); // free-form payload.
};

inline void to_json(nlohmann::json& j, const HookEvent& e) {
    j = nlohmann::json{
        {"source", e.source},
        {"key", e.key},
        {"at", e.at},
        {"detail", e.detail},
    };
}

inline void from_json(const nlohmann::json& j, HookEvent& e) {
    if (auto it = j.find("source"); it != j.end()) it->get_to(e.source);
    if (auto it = j.find("key"); it != j.end()) it->get_to(e.key);
    if (auto it = j.find("at"); it != j.end()) it->get_to(e.at);
    // Absent or null detail keeps the (empty) object default.
    if (auto it = j.find("detail"); it != j.end() && !it->is_null())
        it->get_to(e.detail);
}

// ---- events: append-only broadcast ------------------------------------------------

// Append one event to the `events` region, creating extras / the region on
// first use. Throws std::logic_error on an empty source or key (protocol
// rule 2), and when extras or the region exists with a wrong shape (a
// corrupt reserved region is reported, never silently repaired). The
// append lands at the tail — its position is the event's place in the
// session's total mutation order (protocol rule 4).
inline void publish_hook_event(AgentInputState& state, HookEvent event) {
    if (event.source.empty())
        throw std::logic_error(
            "publish_hook_event: every event needs a non-empty source — "
            "the identity that owns the write");
    if (event.key.empty())
        throw std::logic_error(
            "publish_hook_event: every event needs a non-empty key — "
            "the name of what happened");
    if (!state.extras) state.extras = nlohmann::json::object();
    if (!state.extras->is_object())
        throw std::logic_error("publish_hook_event: extras is not an object");
    nlohmann::json& events = (*state.extras)[kEventsKey];
    if (events.is_null()) {
        events = nlohmann::json::array();
    } else if (!events.is_array()) {
        throw std::logic_error(std::string("publish_hook_event: extras[")
                               + kEventsKey + "] is not an array — the "
                               "reserved region is corrupt");
    }
    events.push_back(nlohmann::json(std::move(event)));
}

// The latest event matching @p key (optionally also @p source), scanning
// from the tail — the read side of last-writer-wins. nullopt when nothing
// matches or the region is absent; malformed entries are skipped, a
// malformed region reads as empty.
inline std::optional<HookEvent> latest_hook_event(
    const AgentInputState& state, std::string_view key,
    std::string_view source = {}) {
    if (!state.extras || !state.extras->is_object()) return std::nullopt;
    const auto region = state.extras->find(kEventsKey);
    if (region == state.extras->end() || !region->is_array())
        return std::nullopt;
    for (auto entry = region->rbegin(); entry != region->rend(); ++entry) {
        if (!entry->is_object()) continue;
        const HookEvent e = entry->get<HookEvent>();
        if (e.key != key) continue;
        if (!source.empty() && e.source != source) continue;
        return e;
    }
    return std::nullopt;
}

// All events in publication order — the session's ordered history,
// optionally filtered by @p source. Same leniency as latest_hook_event on
// malformed regions and entries.
inline std::vector<HookEvent> hook_events(
    const AgentInputState& state, std::string_view source = {}) {
    std::vector<HookEvent> out;
    if (!state.extras || !state.extras->is_object()) return out;
    const auto region = state.extras->find(kEventsKey);
    if (region == state.extras->end() || !region->is_array()) return out;
    for (const auto& entry : *region) {
        if (!entry.is_object()) continue;
        HookEvent e = entry.get<HookEvent>();
        if (!source.empty() && e.source != source) continue;
        out.push_back(std::move(e));
    }
    return out;
}

// ---- external_status: per-writer state slots --------------------------------------

// Overwrite @p source's slot in the `external_status` region with @p status
// — the writer's own latest state, persisted with the session. Creating
// extras / the region on first use. Throws std::logic_error on an empty
// @p source, a non-object @p status (a state document is structured), or a
// corrupt region/extras shape (reported, never silently repaired). A
// writer syncs ONLY its own slot: crosstalk between writers is impossible
// by construction (protocol rule 2).
inline void sync_external_status(AgentInputState& state,
                                 std::string_view source,
                                 nlohmann::json status) {
    if (source.empty())
        throw std::logic_error(
            "sync_external_status: a status sync needs a non-empty source — "
            "the identity that owns the slot");
    if (!status.is_object())
        throw std::logic_error(
            "sync_external_status: a status document must be a JSON object");
    if (!state.extras) state.extras = nlohmann::json::object();
    if (!state.extras->is_object())
        throw std::logic_error(
            "sync_external_status: extras is not an object");
    nlohmann::json& region = (*state.extras)[kExternalStatusKey];
    if (region.is_null()) {
        region = nlohmann::json::object();
    } else if (!region.is_object()) {
        throw std::logic_error(std::string("sync_external_status: extras[")
                               + kExternalStatusKey + "] is not an object — "
                               "the reserved region is corrupt");
    }
    region[source] = std::move(status);
}

// @p source's latest status, or nullopt when the writer never synced (or
// the region is absent/malformed — leniency as everywhere on the read
// side).
inline std::optional<nlohmann::json> external_status(
    const AgentInputState& state, std::string_view source) {
    if (!state.extras || !state.extras->is_object()) return std::nullopt;
    const auto region = state.extras->find(kExternalStatusKey);
    if (region == state.extras->end() || !region->is_object())
        return std::nullopt;
    const auto slot = region->find(source);
    if (slot == region->end() || slot->is_null() || !slot->is_object())
        return std::nullopt;
    return *slot;
}

} // namespace model_io
