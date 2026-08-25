// Interactive DeepSeek chat client with tool calling, built entirely on the
// endpoint module's contract pieces — the "provider wrapper" shape the
// module reserves for compiled interpreter layers, demonstrated here in one
// file for the plain OpenAI-style /chat/completions dialect (DeepSeek's
// native one; the Responses-API compatibility layer is a separate, fuller
// instance of the same shape).
//
// The pieces, one per abstraction level:
//
//   * the data contract (dataclass/model_io.hpp + endpoint_config.hpp):
//     AgentInputState carries the whole conversation — rendered system
//     prompt, registered tools (Invocable), and the turn list the agent
//     loop below appends ReAct cycles to; ModelEndpoint carries base_url,
//     auth and transport headers.
//
//   * chat::ChatChunkHandler — an SSEResponseHandler decoding each SSE
//     event of the stream into one chat::ChatDelta (decode only).
//
//   * chat::ChatReader — a ModelResponseReader folding those deltas into
//     accumulators and, on the `data: [DONE]` sentinel, assembling the
//     contract record: one model_io::MessageItem with content.raw,
//     reasoning.raw, invokes[] (streamed tool calls, arguments parsed) and
//     cost (the usage trailer: prompt/completion/prompt_cache_hit tokens).
//
//   * chat::ChatInterpreter — a ModelRequestInterpreter flattening the
//     AgentInputState back onto the wire: messages[] (system / user /
//     assistant+tool_calls / tool with tool_call_id, correlated through the
//     embedded InvokeReturn provenance record, positional fallback),
//     tools[] from the registered Invocables, stream + include_usage.
//
//   * endpoint::complete — the whole-exchange retry functor: connect, one
//     sse_request exchange, inline drain of the reader, plain retry with
//     backoff on recoverable failures (the reader is clear()-reset and the
//     request re-sent verbatim).
//
// The ReAct agent loop per user turn: run the exchange; a response with no
// invokes is the final answer; otherwise execute each call (this example
// registers one tool, get_current_time), append the results as an
// AgentLoopStep's invoke_returns, and loop. Reasoning deltas stream to
// stderr, content to stdout, live; the step budget bounds the loop.
//
// Build target only; not registered with CTest (it needs a live API key).
#include "endpoint/complete.hpp"
#include "endpoint/http_request_exception.hpp"

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;

namespace chat {

// ---- the streaming view: one delta per SSE event ----------------------------

/// Which incremental channel a ChatDelta belongs to.
enum class ChatDeltaKind {
    Text,           // delta.content — visible assistant text increment.
    ReasoningText,  // delta.reasoning_content — thinking-mode channel.
    ToolCall,       // delta.tool_calls[i] — one fragment of one streamed call
                    // (id / name / args fields; see ChatDelta).
    FinishReason,   // choices[0].finish_reason — the terminal chunk.
    Usage,          // the include_usage trailer (empty choices); extras = the object.
    Done,           // the `data: [DONE]` sentinel — the reader's terminal event.
    Ignored,        // Role deltas, empty fragments, keep-alives: seen, not folded.
};

/// One decoded SSE event of a chat-completion stream. Transient streaming
/// view only, deliberately outside the JSON data contract.
struct ChatDelta {
    ChatDeltaKind kind = ChatDeltaKind::Ignored;
    /// Which streamed tool call this fragment belongs to (the wire's
    /// tool_calls[].index; one call's arguments arrive as many fragments).
    std::size_t tool_index = 0;
    /// The increment bytes — or the finish reason verbatim.
    std::string text;
    /// ToolCall fragments: the fields the wire sent in THIS fragment, each
    /// empty when absent. A single chunk typically carries id + name +
    /// empty arguments together on the first fragment, then arguments
    /// string fragments alone — so the fragment keeps all three sides by
    /// side instead of picking one, and the reader folds whichever are
    /// non-empty.
    std::string id, name, args;
    /// The full usage object, Usage deltas only.
    std::optional<nlohmann::json> extras;
};

// ---- the decoder: SSE event -> ChatDelta -------------------------------------

namespace {

// Guarded nlohmann access: the decode path never throws (a surprising chunk
// surfaces as an Ignored delta, not as a stream fault).

const nlohmann::json* find_object(const nlohmann::json& parent, const char* key) {
    if (auto it = parent.find(key); it != parent.end() && it->is_object())
        return &*it;
    return nullptr;
}

const nlohmann::json* find_array(const nlohmann::json& parent, const char* key) {
    if (auto it = parent.find(key); it != parent.end() && it->is_array())
        return &*it;
    return nullptr;
}

const nlohmann::json* first_choice(const nlohmann::json& chunk) {
    const nlohmann::json* choices = find_array(chunk, "choices");
    if (!choices || choices->empty()) return nullptr;
    const nlohmann::json& choice = (*choices)[0];   // chat completions: n = 1
    return choice.is_object() ? &choice : nullptr;
}

std::string string_field(const nlohmann::json& parent, const char* key) {
    if (auto it = parent.find(key); it != parent.end() && it->is_string())
        return it->get<std::string>();
    return {};
}

std::size_t index_field(const nlohmann::json& parent, const char* key) {
    if (auto it = parent.find(key);
        it != parent.end() && it->is_number_integer())
        return it->get<std::size_t>();
    return 0;
}

ChatDelta ignored(const char* why) {
    ChatDelta delta;
    delta.kind = ChatDeltaKind::Ignored;
    delta.text = why;
    return delta;
}

// One chunk -> one delta. The wire never mixes channels within a chunk in
// practice (role/empty deltas, content or reasoning fragments, tool-call
// fragments, the finish chunk with finish_reason + empty content, and the
// usage trailer with empty choices), so the ordered checks below lose
// nothing; the one REAL overlap — finish_reason riding a chunk whose
// content is "" — is exactly why finish_reason is checked after the
// tool-call fragments but on its own, before text.
ChatDelta decode(const nlohmann::json& chunk) {
    if (const nlohmann::json* choice = first_choice(chunk)) {
        if (const nlohmann::json* delta = find_object(*choice, "delta")) {
            // Streamed tool-call fragments. The FIRST fragment of a call
            // typically carries id + function.name + empty arguments in ONE
            // chunk (the early-return-per-field shape would drop the name
            // riding beside the id — the wire really does co-deliver them),
            // so the delta keeps every present field and the reader folds
            // whichever are non-empty.
            if (const nlohmann::json* calls = find_array(*delta, "tool_calls");
                calls && !calls->empty()) {
                const nlohmann::json& call = (*calls)[0];
                if (call.is_object()) {
                    ChatDelta fragment;
                    fragment.kind = ChatDeltaKind::ToolCall;
                    fragment.tool_index = index_field(call, "index");
                    fragment.id = string_field(call, "id");
                    if (const nlohmann::json* function =
                            find_object(call, "function")) {
                        fragment.name = string_field(*function, "name");
                        fragment.args = string_field(*function, "arguments");
                    }
                    if (!fragment.id.empty() || !fragment.name.empty() ||
                        !fragment.args.empty()) {
                        return fragment;
                    }
                }
                return ignored("empty tool call fragment");
            }
            std::string text = string_field(*delta, "content");
            if (!text.empty())
                return ChatDelta{.kind = ChatDeltaKind::Text, .text = std::move(text)};
            text = string_field(*delta, "reasoning_content");
            if (!text.empty())
                return ChatDelta{.kind = ChatDeltaKind::ReasoningText, .text = std::move(text)};
        }
        std::string reason = string_field(*choice, "finish_reason");
        if (!reason.empty())
            return ChatDelta{.kind = ChatDeltaKind::FinishReason, .text = std::move(reason)};
    }
    // The include_usage trailer arrives with empty choices; regular chunks
    // carry `"usage": null`, which find_object rejects.
    if (const nlohmann::json* usage = find_object(chunk, "usage")) {
        ChatDelta delta;
        delta.kind = ChatDeltaKind::Usage;
        delta.extras = *usage;
        return delta;
    }
    return ignored("empty chunk");
}

} // namespace

/**
 * @brief Decodes a chat-completion SSE stream into one delta per event.
 *
 * Decode only, stateless beyond the framing state it inherits — the
 * accumulation into the contract record lives one layer up, in ChatReader.
 * Exception-total like the module's other decoders: a malformed payload is
 * an Ignored delta, never a stream fault.
 */
class ChatChunkHandler final
    : public endpoint::SSEResponseHandler<ChatDelta> {
public:
    using endpoint::SSEResponseHandler<ChatDelta>::SSEResponseHandler;

protected:
    ChatDelta _handle_message(std::span<const LineInfo> message) override {
        for (const auto& [field, value] : message) {
            if (field != "data") continue;   // ignore event:/id:/comments/etc.

            if (value == "[DONE]") return ChatDelta{.kind = ChatDeltaKind::Done};

            nlohmann::json chunk;
            try {
                chunk = nlohmann::json::parse(value);
            } catch (const nlohmann::json::parse_error&) {
                return ignored("undecodable payload");
            }
            if (!chunk.is_object()) return ignored("non-object payload");
            return decode(chunk);
        }
        return ignored("no data field");
    }
};

// ---- the consumer: deltas in, one MessageItem out ----------------------------

namespace {

// The wire carries tool arguments as a JSON string; fragments concatenated,
// parse leniently — the docs warn the model may emit invalid JSON or stray
// fields, so a non-object parse keeps the raw string (round-trips verbatim).
nlohmann::json parse_arguments(const std::string& args) {
    if (!args.empty()) {
        try {
            if (nlohmann::json parsed = nlohmann::json::parse(args);
                parsed.is_object()) {
                return parsed;
            }
        } catch (const nlohmann::json::exception&) {
            // fall through: keep the raw string
        }
    }
    return nlohmann::json::object();
}

std::uint64_t uint_field(const nlohmann::json& parent, const char* key) {
    if (auto it = parent.find(key);
        it != parent.end() && it->is_number_integer())
        return it->get<std::uint64_t>();
    return 0;
}

} // namespace

/**
 * @brief Consumes a chat-completion delta stream into one MessageItem.
 *
 * Folds each ChatDelta into plain string accumulators (per-index for the
 * streamed tool calls), and on the [DONE] sentinel assembles the contract
 * record — the same shape the Responses layer produces, minus the parts the
 * chat dialect has no wire form for:
 *
 *   delta.content           -> content.raw
 *   delta.reasoning_content -> reasoning.raw           (thinking models)
 *   tool_calls fragments    -> invokes[] (id/name/arguments parsed)
 *   usage trailer           -> cost (TokenCost: DeepSeek reports
 *                              prompt_cache_hit_tokens -> cache_hit)
 *   finish_reason           -> extras["finish_reason"]
 *
 * Reuse: clear() rewinds this layer's accumulators on top of the base's
 * stream/handler reset, so the retry functor below can re-read a whole
 * response through the same reader.
 */
class ChatReader final : public endpoint::ModelResponseReader<ChatDelta> {
public:
    /// Create the decoder this reader will drain, and adopt it.
    explicit ChatReader(boost::asio::any_io_executor executor)
        : ModelResponseReader(
              std::make_shared<ChatChunkHandler>(std::move(executor))) {}

    const model_io::MessageItem& response() const noexcept override {
        return _response;
    }

    void clear() override {
        endpoint::ModelResponseReader<ChatDelta>::clear();
        _content.clear();
        _reasoning.clear();
        _finish_reason.clear();
        _calls.clear();
        _usage.reset();
        _response = model_io::MessageItem{};
    }

protected:
    void _accumulate(const ChatDelta& delta) override {
        switch (delta.kind) {
            case ChatDeltaKind::Text:
                _content += delta.text;
                break;
            case ChatDeltaKind::ReasoningText:
                _reasoning += delta.text;
                break;
            case ChatDeltaKind::ToolCall: {
                // Fold whichever sides the fragment carried; += tolerates a
                // provider splitting even the id or name across chunks.
                ToolCall& call = _calls[delta.tool_index];
                if (!delta.id.empty()) call.id += delta.id;
                if (!delta.name.empty()) call.name += delta.name;
                if (!delta.args.empty()) call.args += delta.args;
                break;
            }
            case ChatDeltaKind::FinishReason:
                _finish_reason = delta.text;
                break;
            case ChatDeltaKind::Usage:
                _usage = delta.extras;
                break;
            case ChatDeltaKind::Done:
            case ChatDeltaKind::Ignored:
                break;
        }
    }

    bool _is_terminal(const ChatDelta& delta) const override {
        return delta.kind == ChatDeltaKind::Done;
    }

    void _assemble() override {
        _response = model_io::MessageItem{};
        _response.type = model_io::MessageItemType::ModelResponse;
        _response.role = "assistant";
        _response.content.type = model_io::ContentType::Text;
        _response.content.raw = _content;

        if (!_reasoning.empty()) {
            _response.reasoning = model_io::Content{};
            _response.reasoning->type = model_io::ContentType::Text;
            _response.reasoning->raw = _reasoning;
        }

        if (!_calls.empty()) {
            std::vector<model_io::InvokeQuery> invokes;
            invokes.reserve(_calls.size());
            for (const auto& [index, call] : _calls) {   // wire index order
                model_io::InvokeQuery query;
                query.id = call.id;
                query.name = call.name;
                query.arguments = parse_arguments(call.args);
                invokes.push_back(std::move(query));
            }
            _response.invokes = std::move(invokes);
        }

        if (_usage && _usage->is_object()) {
            model_io::TokenCost cost;
            cost.prompt = uint_field(*_usage, "prompt_tokens");
            cost.generated = uint_field(*_usage, "completion_tokens");
            cost.cache_hit = uint_field(*_usage, "prompt_cache_hit_tokens");
            _response.cost = cost;
        }

        if (!_finish_reason.empty()) {
            _response.extras =
                nlohmann::json{{"finish_reason", _finish_reason}};
        }
    }

private:
    struct ToolCall {
        std::string id, name, args;   // args: concatenated JSON string fragments
    };

    std::string _content, _reasoning, _finish_reason;
    std::map<std::size_t, ToolCall> _calls;
    std::optional<nlohmann::json> _usage;
    model_io::MessageItem _response;
};

// ---- the request half: AgentInputState -> POST /chat/completions -------------

namespace {

using nlohmann::json;
namespace http = boost::beast::http;

// The wire carries tool arguments as a JSON string; an InvokeQuery.arguments
// that already is one passes through, anything else is dumped.
std::string wire_arguments(const json& arguments) {
    if (arguments.is_string()) return arguments.get<std::string>();
    return arguments.dump();
}

std::string derived_role(const model_io::MessageItem& item) {
    if (!item.role.empty()) return item.role;
    switch (item.type) {
        case model_io::MessageItemType::ModelResponse: return "assistant";
        case model_io::MessageItemType::InvokeReturn: return "tool";
        default: return "user";
    }
}

// The wire "tool_call_id" for one tool result — the interface contract's
// correlation ladder: the embedded provenance record's query.id first, then
// positional alignment with the parent response's invokes, then omitted.
std::string tool_call_id(
    const model_io::MessageItem& result, std::size_t position,
    const std::optional<std::vector<model_io::InvokeQuery>>& invokes) {
    if (result.invoke_return && !result.invoke_return->query.id.empty())
        return result.invoke_return->query.id;
    if (invokes && position < invokes->size())
        return (*invokes)[position].id;
    return {};
}

json assistant_message(const model_io::MessageItem& response) {
    json message;
    message["role"] = derived_role(response);
    // Mirror the SDK replay of response.choices[0].message: content rides
    // as a plain string, empty on pure tool-call turns (the docs' own
    // multi-turn samples replay exactly this shape).
    message["content"] = response.content.raw;
    // Thinking mode is ON by default, and the thinking-mode guide is
    // explicit: a request carrying tools MUST replay every intermediate
    // assistant message's reasoning_content verbatim — even when that
    // response made no tool calls — or the API answers 400. A no-tools
    // request simply ignores the field, so sending it unconditionally is
    // safe. (The 400's own message misreports this as a content-type
    // complaint; the missing reasoning_content is the actual trigger.)
    if (response.reasoning && !response.reasoning->raw.empty()) {
        message["reasoning_content"] = response.reasoning->raw;
    }
    if (response.invokes && !response.invokes->empty()) {
        json calls = json::array();
        for (const model_io::InvokeQuery& query : *response.invokes) {
            calls.push_back(json{
                {"id", query.id},
                {"type", "function"},
                {"function", json{
                    {"name", query.name},
                    {"arguments", wire_arguments(query.arguments)},
                }},
            });
        }
        message["tool_calls"] = std::move(calls);
    }
    return message;
}

json tool_message(
    const model_io::MessageItem& result, std::size_t position,
    const std::optional<std::vector<model_io::InvokeQuery>>& invokes) {
    const model_io::InvokeReturn* record =
        result.invoke_return ? &*result.invoke_return : nullptr;
    json message;
    message["role"] = "tool";
    if (std::string id = tool_call_id(result, position, invokes); !id.empty())
        message["tool_call_id"] = std::move(id);
    // content is the contract's canonical payload position; the embedded
    // record's output carries the same bytes and serves as fallback.
    message["content"] = (result.content.raw.empty() && record)
        ? record->output.raw
        : result.content.raw;
    return message;
}

} // namespace

/**
 * @brief Flattens an AgentInputState into one /chat/completions request.
 *
 * The chat-dialect twin of the Responses layer's interpreter, minus the
 * round-trip captures (this dialect has no server-side item records to
 * re-emit): system prompt rendered to messages[0], turns flattened to
 * user / assistant(+tool_calls) / tool messages, tools registered from the
 * Invocables. Generation JSON passes through verbatim except the
 * builder-owned keys (model check, messages, tools, stream,
 * stream_options), which win. Pure and synchronous, like the contract
 * demands — the two hard errors aside (no model name, hostless base_url).
 */
class ChatInterpreter final : public endpoint::ModelRequestInterpreter {
public:
    HttpRequest build_request(
        const model_io::AgentInputState& conversation,
        const model_io::ModelEndpoint& endpoint,
        const nlohmann::json& generation) override;
};

endpoint::ModelRequestInterpreter::HttpRequest ChatInterpreter::build_request(
    const model_io::AgentInputState& conversation,
    const model_io::ModelEndpoint& endpoint,
    const nlohmann::json& generation) {
    // Hard error #1 (the interface contract): a non-empty model name.
    const auto model = generation.find("model");
    if (model == generation.end() || !model->is_string() ||
        model->get<std::string>().empty()) {
        throw HttpRequestException(
            HttpRequestException::Stage::CreateRequest,
            "generation carries no non-empty \"model\"");
    }
    // Hard error #2: the base_url must resolve to a host.
    const endpoint::ResolvedEndpoint where = endpoint::resolve_endpoint(endpoint);

    json body = generation;   // verbatim passthrough; builder keys below win

    json messages = json::array();
    const std::string system = conversation.system_prompt.render().markdown;
    if (!system.empty()) {
        messages.push_back(json{{"role", "system"}, {"content", system}});
    }
    for (const model_io::UserLoopStep& turn : conversation.turns) {
        if (turn.user_input.type == model_io::MessageItemType::InvokeReturn) {
            // Lenient: a tool result in a user position still maps through
            // its embedded record — the record says what it is.
            messages.push_back(tool_message(turn.user_input, 0, std::nullopt));
        } else {
            messages.push_back(json{
                {"role", derived_role(turn.user_input)},
                {"content", turn.user_input.content.raw},
            });
        }
        for (const model_io::AgentLoopStep& step : turn.agent_loop_step) {
            messages.push_back(assistant_message(step.model_response));
            if (step.invoke_returns) {
                for (std::size_t index = 0;
                     index < step.invoke_returns->size(); ++index) {
                    messages.push_back(tool_message(
                        (*step.invoke_returns)[index], index,
                        step.model_response.invokes));
                }
            }
        }
    }
    body["messages"] = std::move(messages);

    if (!conversation.tools.empty()) {
        json tools = json::array();
        for (const model_io::Invocable& tool : conversation.tools) {
            tools.push_back(json{
                {"type", "function"},
                {"function", json{
                    {"name", tool.name},
                    {"description", tool.description},
                    {"parameters", tool.argument_schema},
                }},
            });
        }
        body["tools"] = std::move(tools);
    }

    // Builder-owned keys win: the transport speaks SSE only, and the usage
    // trailer (the empty-choices chunk before [DONE]) is what populates
    // MessageItem::cost on the reader side.
    body["stream"] = true;
    body["stream_options"] = json{{"include_usage", true}};

    HttpRequest request{http::verb::post, where.target, 11};
    // RFC 9110 §7.2: the Host authority carries the port when non-default.
    request.set(http::field::host, where.authority());
    endpoint::apply_transport_headers(request, endpoint);
    request.set(http::field::accept, "text/event-stream");
    request.set(http::field::content_type, "application/json");
    request.body() = body.dump();
    request.prepare_payload();
    return request;
}

} // namespace chat

// ---- the one registered tool: current system time ----------------------------

namespace {

// Default model. Override at runtime with the DEEPSEEK_MODEL environment
// variable.
const char* model_name() {
    if (const char* env = std::getenv("DEEPSEEK_MODEL")) return env;
    return "deepseek-v4-flash";
}

/// The tool registration (dataclass/model_io.hpp): the wire triple providers
/// expect — name, description, JSON-Schema for the arguments. This tool is a
/// plain in-process, zero-argument, read-only function.
model_io::Invocable current_time_tool() {
    model_io::Invocable tool;
    tool.name = "get_current_time";
    tool.description =
        "Get the current system date and time as an ISO-8601 UTC timestamp "
        "(e.g. 2026-08-24T12:34:56Z). Takes no arguments.";
    tool.argument_schema = nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json::object()},
        {"required", nlohmann::json::array()},
        {"additionalProperties", false},
    };
    return tool;
}

std::string current_time_utc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds =
        std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char formatted[32];
    std::strftime(formatted, sizeof formatted, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return formatted;
}

/// Execute one tool call and shape the result as the contract wants it: the
/// payload in content (the canonical position, role "tool") PLUS the
/// originating call embedded as the InvokeReturn provenance record, whose
/// query.id is what the next request correlates the result by.
///
/// Dispatch is by name; a real runtime would additionally consult the
/// registry's InvokeType/InvokeSecurity metadata before executing (a wire
/// InvokeQuery defaults to DefaultDeny — the registry, not the model, is
/// where trust decisions live). An unknown tool is reported TO the model as
/// the result text, so it can recover in the next cycle.
model_io::MessageItem execute_tool(const model_io::InvokeQuery& query) {
    model_io::InvokeReturn record;
    record.query = query;
    record.output.type = model_io::ContentType::Text;

    if (query.name == "get_current_time") {
        record.output.raw = current_time_utc();
    } else {
        record.output.raw = "error: unknown tool \"" + query.name + "\"";
    }

    model_io::MessageItem item;
    item.type = model_io::MessageItemType::InvokeReturn;
    item.role = "tool";
    item.content = record.output;
    item.invoke_return = std::move(record);
    return item;
}

// ---- one user turn: the ReAct agent loop --------------------------------------

constexpr std::size_t kMaxAgentSteps = 8;

/// Run the agent loop for the last user turn: exchange; a response with no
/// invokes is the final answer; otherwise execute the calls, append the
/// results as invoke_returns, repeat — each exchange a fresh reader, the
/// whole turn a growing AgentInputState the interpreter flattens per
/// exchange (which is how the tool history rides along).
asio::awaitable<void> run_turn(
    endpoint::complete& complete_model,
    chat::ChatInterpreter& interpreter,
    const model_io::ModelEndpoint& endpoint_config,
    const endpoint::ResolvedEndpoint& resolved,
    model_io::AgentInputState& state) {
    model_io::UserLoopStep& turn = state.turns.back();

    for (std::size_t step = 0; step < kMaxAgentSteps; ++step) {
        auto reader = std::make_shared<chat::ChatReader>(
            co_await asio::this_coro::executor);
        // The live view: content to stdout, thinking to stderr, as the
        // exchange streams. NOTE: hooks survive the retry functor's
        // clear() reset, so a retried exchange replays its deltas through
        // this hook — earlier partial text may print twice on a retry.
        reader->add_hook([](const chat::ChatDelta& delta) {
            if (delta.kind == chat::ChatDeltaKind::Text &&
                !delta.text.empty())
                std::cout << delta.text << std::flush;
            if (delta.kind == chat::ChatDeltaKind::ReasoningText &&
                !delta.text.empty())
                std::cerr << delta.text << std::flush;
        });

        const nlohmann::json generation{{"model", model_name()}};
        auto request = interpreter.build_request(
            state, endpoint_config, generation);

        model_io::MessageItem item =
            co_await complete_model.operator()<chat::ChatDelta>(
                resolved, std::move(request), reader,
                endpoint::sse_request<chat::ChatDelta>);

        if (!item.invokes || item.invokes->empty()) {
            // Final answer: already streamed live by the hook. Close the
            // line, report the exchange's token accounting, keep the turn.
            std::cout << "\n";
            if (item.cost) {
                std::cerr << "[cost] prompt=" << item.cost->prompt
                          << " generated=" << item.cost->generated
                          << " cache_hit=" << item.cost->cache_hit << "\n";
            }
            turn.agent_loop_step.push_back(model_io::AgentLoopStep{
                .model_response = std::move(item)});
            co_return;
        }

        // ReAct: execute every call, push the results back as one cycle.
        std::vector<model_io::MessageItem> results;
        results.reserve(item.invokes->size());
        for (const model_io::InvokeQuery& query : *item.invokes) {
            // The assembled call verbatim (id / name / parsed arguments) —
            // the parsed wire view, so decode gaps show up at a glance.
            std::cout << "\n  [tool] id=\"" << query.id << "\" name=\""
                      << query.name << "\" arguments="
                      << query.arguments.dump();
            model_io::MessageItem result = execute_tool(query);
            std::cout << " -> " << result.content.raw << "\n";
            results.push_back(std::move(result));
        }

        model_io::AgentLoopStep cycle;
        cycle.model_response = std::move(item);
        cycle.invoke_returns = std::move(results);
        turn.agent_loop_step.push_back(std::move(cycle));
    }
    std::cerr << "agent loop exhausted its step budget before a final "
                 "answer\n";
}

} // namespace

int main() {
    std::cout << "=== DeepSeek chat-completion example "
                 "(streaming, one tool) ===\n";

    std::string base_url;
    std::cout << "base_url [https://api.deepseek.com]: ";
    if (!std::getline(std::cin, base_url) || base_url.empty())
        base_url = "https://api.deepseek.com";

    std::string api_key;
    std::cout << "api_key: ";
    if (!std::getline(std::cin, api_key) || api_key.empty()) {
        std::cerr << "no api_key provided; exiting.\n";
        return 1;
    }

    // The deployment half (dataclass/endpoint_config.hpp): where + how.
    model_io::ModelEndpoint endpoint_config;
    endpoint_config.base_url = base_url;
    endpoint_config.request_path = "/chat/completions";
    endpoint_config.auth.scheme = model_io::AuthScheme::Bearer;
    endpoint_config.auth.api_key = api_key;
    endpoint_config.user_agent = "simplex-cpp-deepseek-example";

    endpoint::ResolvedEndpoint resolved;
    try {
        resolved = endpoint::resolve_endpoint(endpoint_config);
    } catch (const std::exception& error) {
        std::cerr << "bad base_url: " << error.what() << "\n";
        return 1;
    }

    // The conversation half (dataclass/model_io.hpp): system prompt,
    // registered tools, and the turn list the agent loop appends to.
    model_io::AgentInputState state;
    state.system_prompt.add_section(
        "persona", "", "You are a helpful assistant.",
        model_io::SectionStability::Immutable);
    state.tools.push_back(current_time_tool());

    std::cout << "using " << resolved.host << ":" << resolved.port
              << resolved.target << " (model: " << model_name() << ")\n";
    std::cout << "tool registered: get_current_time "
                 "(try: \"what time is it?\")\n";
    std::cout << "reasoning_content (if any) is streamed to stderr.\n";
    std::cout << "empty line to quit.\n";

    // The retry engine: one instance for the whole session, called once per
    // exchange (fresh connect + reader reset + request re-send per attempt).
    asio::io_context io;
    endpoint::complete complete_model{io.get_executor()};
    chat::ChatInterpreter interpreter;

    std::string line;
    while (std::cout << "\nyou> " && std::getline(std::cin, line)) {
        if (line.empty()) break;

        model_io::UserLoopStep turn;
        turn.user_input.type = model_io::MessageItemType::UserInput;
        turn.user_input.role = "user";
        turn.user_input.content.raw = line;
        state.turns.push_back(std::move(turn));

        try {
            auto future = asio::co_spawn(
                io,
                run_turn(complete_model, interpreter, endpoint_config,
                         resolved, state),
                asio::use_future);
            io.restart();   // a prior turn's run() drained the context
            io.run();
            future.get();
        } catch (const std::exception& error) {
            std::cerr << "turn failed: " << error.what() << "\n";
            state.turns.pop_back();   // drop the unanswered turn
        }
    }

    return 0;
}
