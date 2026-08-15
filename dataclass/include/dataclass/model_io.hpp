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
// MessageItem -> AgentLoopStep -> UserLoopStep.
//
// The tool-definition record (ToolSchema) and the session-level container
// (system prompt + user + registered tools + turns) are intentionally not
// part of the contract yet; both will be defined once these types settle.
//
// Overall model-I/O management logic
// ----------------------------------
// The conversation is organised as a two-tier loop:
//
//   UserLoopStep
//   +-- user_input      : MessageItem         (one user message)
//   +-- agent_loop_step : vector<MessageItem> <- the "agent loop"
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
    std::vector<MessageItem> agent_loop_step;
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

} // namespace model_io
