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
// MessageItem -> AgentLoopStep -> UserLoopStep -> Invocable. The session
// container AgentInputState closes the file and is the ONE composite without
// a serialisation pair: it embeds PromptTemplate (prompt_template.hpp), which
// is text management only and deliberately outside the JSON contract.
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
//     invoke_returns  — the tool results produced by the calls in that response
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

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dataclass/prompt_template.hpp"

namespace model_io {

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
};

inline void to_json(nlohmann::json& j, const Content& c) {
    j = nlohmann::json{{"type", c.type}, {"raw", c.raw}};
}

inline void from_json(const nlohmann::json& j, Content& c) {
    if (auto it = j.find("type"); it != j.end()) it->get_to(c.type);
    if (auto it = j.find("raw"); it != j.end()) it->get_to(c.raw);
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
    if (auto it = j.find("extras"); it != j.end()) q.extras = *it;
    else q.extras.reset();
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
    if (auto it = j.find("extras"); it != j.end()) r.extras = *it;
    else r.extras.reset();
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
// `reasoning`/`invokes` on model responses, the result in `content` for invoke
// returns).
struct MessageItem {
    MessageItemType type = MessageItemType::UserInput;
    std::string role;
    Content content;
    std::optional<Content> reasoning, action_status;
    std::optional<std::vector<InvokeQuery>> invokes;
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
    if (m.extras) j["extras"] = *m.extras;
}

inline void from_json(const nlohmann::json& j, MessageItem& m) {
    if (auto it = j.find("type"); it != j.end()) it->get_to(m.type);
    if (auto it = j.find("role"); it != j.end()) it->get_to(m.role);
    if (auto it = j.find("content"); it != j.end()) it->get_to(m.content);
    if (auto it = j.find("reasoning"); it != j.end())
        m.reasoning = it->get<Content>();
    else m.reasoning.reset();
    if (auto it = j.find("action_status"); it != j.end())
        m.action_status = it->get<Content>();
    else m.action_status.reset();
    if (auto it = j.find("invokes"); it != j.end())
        m.invokes = it->get<std::vector<InvokeQuery>>();
    else m.invokes.reset();
    if (auto it = j.find("extras"); it != j.end()) m.extras = *it;
    else m.extras.reset();
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
    if (auto it = j.find("invoke_returns"); it != j.end())
        s.invoke_returns = it->get<std::vector<MessageItem>>();
    else s.invoke_returns.reset();
    if (auto it = j.find("extras"); it != j.end()) s.extras = *it;
    else s.extras.reset();
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
    if (auto it = j.find("extras"); it != j.end()) u.extras = *it;
    else u.extras.reset();
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
    if (auto it = j.find("remote_type"); it != j.end())
        v.remote_type = it->get<std::string>();
    else v.remote_type.reset();
    if (auto it = j.find("extras"); it != j.end()) v.extras = *it;
    else v.extras.reset();
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
//   +-- system_prompt : PromptTemplate
//   |      The system prompt as managed markdown (prompt_template.hpp — text
//   |      management only, deliberately outside the JSON contract). Laid
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
//   |        content        : Content  type : ContentType (text | binary |
//   |                               external_ref) — how `raw` is encoded;
//   |                               raw  : string payload
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
//   |        extras?        : optional<json>
//   |
//   +-- extras : optional<json>
//          Escape hatch for the session state itself (provider-specific or
//          experimental knobs the interpreter should pass through).
//
// Serialisation: NONE, deliberately. Every other composite in this file has
// an ADL to_json/from_json pair; AgentInputState does not, because it embeds
// PromptTemplate, which by design has no JSON form. It is an in-memory
// assembly point: to persist a session, serialise the members that have
// pairs (tools, turns) and store system_prompt.render().markdown — not this
// struct.
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
//     tool-result messages
//   - extras -> interpreter/provider-specific request parameters
//   Everything else a request needs — model name, base_url, temperature,
//   stream, max_tokens, ... — comes from the interpreter's provider/endpoint
//   configuration, NOT from this struct (see Scope above).
// The mapping lives in the interpreter, never here: this struct is pure data.
struct AgentInputState {
    // The system prompt: managed markdown, rendered by the interpreter at
    // request-build time (never serialised).
    PromptTemplate system_prompt;
    // Tools registered for this session — what the model may invoke.
    std::vector<Invocable> tools;
    // The conversation so far, oldest user turn first.
    std::vector<UserLoopStep> turns;
    // Escape hatch: provider-specific / experimental session-level fields.
    std::optional<nlohmann::json> extras;
};

} // namespace model_io
