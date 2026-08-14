#pragma once

//
// model_io.hpp — Model I/O data contract
// =======================================
//
// Data structures describing a conversation with an LLM model and the tool
// invocations (ReAct) it performs. These types are the shared contract the
// project roadmap reserves for the future "model adapter" and "agentloop"
// modules. Every record derives from `dataclass::Serializable`
// (dataclass/serializable.hpp) and implements `to_json()`/`from_json()` member
// functions, so serialization travels with the type itself and every value is
// usable through the unified `dataclass::Serializable&` interface. On top of
// the interface each record supports `X{json}` construction and `x = json`
// assignment, and its to_json/from_json bodies are written in ONE uniform
// style: every field goes through the field codec family
// dataclass::to_json/from_json/to_json_array/from_json_array, with the
// dataclass::optional token marking optional fields (see serializable.hpp).
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

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dataclass/serializable.hpp"

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
struct Content : public dataclass::Serializable {
    Content() = default;
    explicit Content(const nlohmann::json& j) { from_json(j); }
    using dataclass::Serializable::operator=;

    ContentType type = ContentType::Text;
    std::string raw;

    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

inline nlohmann::json Content::to_json() const {
    nlohmann::json j;
    dataclass::to_json(j, "type", type);
    dataclass::to_json(j, "raw", raw);
    return j;
}

inline void Content::from_json(const nlohmann::json& j) {
    dataclass::from_json(j, "type", type);
    dataclass::from_json(j, "raw", raw);
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
struct InvokeQuery : public dataclass::Serializable {
    InvokeQuery() = default;
    explicit InvokeQuery(const nlohmann::json& j) { from_json(j); }
    using dataclass::Serializable::operator=;

    InvokeType type = InvokeType::ReadOnly;
    InvokeSecurity security = InvokeSecurity::DefaultDeny;
    std::string id, name;
    nlohmann::json arguments = nlohmann::json::object();
    std::optional<nlohmann::json> extras;

    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

inline nlohmann::json InvokeQuery::to_json() const {
    nlohmann::json j;
    dataclass::to_json(j, "type", type);
    dataclass::to_json(j, "security", security);
    dataclass::to_json(j, "id", id);
    dataclass::to_json(j, "name", name);
    dataclass::to_json(j, "arguments", arguments);
    dataclass::to_json(j, "extras", extras, dataclass::optional);
    return j;
}

inline void InvokeQuery::from_json(const nlohmann::json& j) {
    dataclass::from_json(j, "type", type);
    dataclass::from_json(j, "security", security);
    dataclass::from_json(j, "id", id);
    dataclass::from_json(j, "name", name);
    dataclass::from_json(j, "arguments", arguments);
    dataclass::from_json(j, "extras", extras, dataclass::optional);
}

// The result of executing an InvokeQuery.
struct InvokeReturn : public dataclass::Serializable {
    InvokeReturn() = default;
    explicit InvokeReturn(const nlohmann::json& j) { from_json(j); }
    using dataclass::Serializable::operator=;

    InvokeQuery query;
    Content output;
    std::optional<nlohmann::json> extras;

    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

inline nlohmann::json InvokeReturn::to_json() const {
    nlohmann::json j;
    dataclass::to_json(j, "query", query);
    dataclass::to_json(j, "output", output);
    dataclass::to_json(j, "extras", extras, dataclass::optional);
    return j;
}

inline void InvokeReturn::from_json(const nlohmann::json& j) {
    dataclass::from_json(j, "query", query);
    dataclass::from_json(j, "output", output);
    dataclass::from_json(j, "extras", extras, dataclass::optional);
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
struct MessageItem : public dataclass::Serializable {
    MessageItem() = default;
    explicit MessageItem(const nlohmann::json& j) { from_json(j); }
    using dataclass::Serializable::operator=;

    MessageItemType type = MessageItemType::UserInput;
    std::string role;
    Content content;
    std::optional<Content> reasoning, action_status;
    std::optional<std::vector<InvokeQuery>> invokes;
    std::optional<nlohmann::json> extras;

    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

inline nlohmann::json MessageItem::to_json() const {
    nlohmann::json j;
    dataclass::to_json(j, "type", type);
    dataclass::to_json(j, "role", role);
    dataclass::to_json(j, "content", content);
    dataclass::to_json(j, "reasoning", reasoning, dataclass::optional);
    dataclass::to_json(j, "action_status", action_status, dataclass::optional);
    dataclass::to_json_array(j, "invokes", invokes, dataclass::optional);
    dataclass::to_json(j, "extras", extras, dataclass::optional);
    return j;
}

inline void MessageItem::from_json(const nlohmann::json& j) {
    dataclass::from_json(j, "type", type);
    dataclass::from_json(j, "role", role);
    dataclass::from_json(j, "content", content);
    dataclass::from_json(j, "reasoning", reasoning, dataclass::optional);
    dataclass::from_json(j, "action_status", action_status, dataclass::optional);
    dataclass::from_json_array(j, "invokes", invokes, dataclass::optional);
    dataclass::from_json(j, "extras", extras, dataclass::optional);
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
struct AgentLoopStep : public dataclass::Serializable {
    AgentLoopStep() = default;
    explicit AgentLoopStep(const nlohmann::json& j) { from_json(j); }
    using dataclass::Serializable::operator=;

    MessageItem model_response;
    std::optional<std::vector<MessageItem>> invoke_returns;
    std::optional<nlohmann::json> extras;
    // Trim preference — order in which this step is dropped to fit the budget.
    RetainPriority retain_priority = RetainPriority::Normal;

    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

inline nlohmann::json AgentLoopStep::to_json() const {
    nlohmann::json j;
    dataclass::to_json(j, "model_response", model_response);
    dataclass::to_json_array(j, "invoke_returns", invoke_returns,
                             dataclass::optional);
    dataclass::to_json(j, "extras", extras, dataclass::optional);
    dataclass::to_json(j, "retain_priority", retain_priority);
    return j;
}

inline void AgentLoopStep::from_json(const nlohmann::json& j) {
    dataclass::from_json(j, "model_response", model_response);
    dataclass::from_json_array(j, "invoke_returns", invoke_returns,
                               dataclass::optional);
    dataclass::from_json(j, "extras", extras, dataclass::optional);
    dataclass::from_json(j, "retain_priority", retain_priority);
}

// One user-loop step: a user message plus the agent steps it triggered.
struct UserLoopStep : public dataclass::Serializable {
    UserLoopStep() = default;
    explicit UserLoopStep(const nlohmann::json& j) { from_json(j); }
    using dataclass::Serializable::operator=;

    MessageItem user_input;
    std::vector<MessageItem> agent_loop_step;
    std::optional<nlohmann::json> extras;
    // Compact-selection preference — bias for summarising vs. keeping this turn.
    RetainPriority retain_priority = RetainPriority::Normal;

    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

inline nlohmann::json UserLoopStep::to_json() const {
    nlohmann::json j;
    dataclass::to_json(j, "user_input", user_input);
    dataclass::to_json_array(j, "agent_loop_step", agent_loop_step);
    dataclass::to_json(j, "extras", extras, dataclass::optional);
    dataclass::to_json(j, "retain_priority", retain_priority);
    return j;
}

inline void UserLoopStep::from_json(const nlohmann::json& j) {
    dataclass::from_json(j, "user_input", user_input);
    dataclass::from_json_array(j, "agent_loop_step", agent_loop_step);
    dataclass::from_json(j, "extras", extras, dataclass::optional);
    dataclass::from_json(j, "retain_priority", retain_priority);
}

} // namespace model_io
