#include "llm/chat_completions/reader.hpp"

#include <cstdint>
#include <utility>

namespace llm::chat_completions {

namespace {

std::string get_string(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    return it != object.end() && it->is_string()
        ? it->get<std::string>()
        : std::string();
}

std::optional<std::uint64_t> get_uint64(
    const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return std::nullopt;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_unsigned()) return std::nullopt;
    return it->get<std::uint64_t>();
}

} // namespace

void ChatCompletionsReader::clear() {
    endpoint::ModelResponseReader<ChatCompletionsDelta>::clear();
    _role = "assistant";
    _content.clear();
    _refusal.clear();
    _tool_calls.clear();
    _finish_reason.clear();
    _completion_id.clear();
    _model.clear();
    _usage.reset();
    _error.reset();
    _response = model_io::MessageItem{};
    _status = ChatCompletionStatus::Streaming;
}

void ChatCompletionsReader::_accumulate(
    const ChatCompletionsDelta& delta) {
    if (!delta.role.empty()) _role = delta.role;
    _content += delta.content;
    _refusal += delta.refusal;
    if (!delta.finish_reason.empty()) _finish_reason = delta.finish_reason;
    if (delta.usage) _usage = delta.usage;

    for (const auto& fragment : delta.tool_calls) {
        ToolCallState& call = _tool_calls[fragment.index];
        call.id += fragment.id;
        if (!fragment.type.empty()) call.type = fragment.type;
        if (!fragment.name.empty()) call.name += fragment.name;
        call.arguments += fragment.arguments;
    }

    if (delta.extras && delta.extras->is_object()) {
        const std::string id = get_string(*delta.extras, "id");
        const std::string model = get_string(*delta.extras, "model");
        if (!id.empty()) _completion_id = id;
        if (!model.empty()) _model = model;
        if (const auto error = delta.extras->find("error");
            error != delta.extras->end() && error->is_object()) {
            _error = *error;
        }
    }

    if (delta.error) {
        _status = ChatCompletionStatus::Failed;
    } else if (delta.done) {
        if (_finish_reason == "length") {
            _status = ChatCompletionStatus::LengthLimited;
        } else if (_finish_reason == "content_filter") {
            _status = ChatCompletionStatus::ContentFiltered;
        } else if (_finish_reason == "stop" ||
                   _finish_reason == "tool_calls" ||
                   _finish_reason == "function_call") {
            _status = ChatCompletionStatus::Completed;
        } else {
            _status = ChatCompletionStatus::Failed;
        }
    }
}

void ChatCompletionsReader::_assemble() {
    model_io::MessageItem result;
    result.type = model_io::MessageItemType::ModelResponse;
    result.role = _role.empty() ? "assistant" : _role;

    model_io::Content content;
    content.type = model_io::ContentType::Text;
    content.raw = _content;
    if (!_refusal.empty()) {
        if (content.raw.empty()) {
            content.raw = _refusal; // readable fallback for refusal-only output
        }
        // Always retain the wire distinction. The interpreter needs this on
        // the next turn to replay `refusal` instead of ordinary assistant text.
        content.extras = nlohmann::json{{"refusal", _refusal}};
    }
    result.content.push_back(std::move(content));

    if (!_tool_calls.empty()) {
        std::vector<model_io::InvokeQuery> invokes;
        invokes.reserve(_tool_calls.size());
        for (const auto& [index, call] : _tool_calls) {
            model_io::InvokeQuery query;
            query.id = call.id;
            query.name = call.name;
            if (!call.arguments.empty()) {
                nlohmann::json parsed =
                    nlohmann::json::parse(call.arguments, nullptr, false);
                query.arguments = parsed.is_discarded()
                    ? nlohmann::json(call.arguments)
                    : std::move(parsed);
            }
            query.extras = nlohmann::json{
                {"index", index},
                {"type", call.type.empty() ? "function" : call.type},
            };
            invokes.push_back(std::move(query));
        }
        result.invokes = std::move(invokes);
    }

    if (_usage && _usage->is_object()) {
        model_io::TokenCost cost;
        bool has_cost = false;
        if (auto value = get_uint64(*_usage, "prompt_tokens")) {
            cost.prompt = *value;
            has_cost = true;
        }
        if (auto value = get_uint64(*_usage, "completion_tokens")) {
            cost.generated = *value;
            has_cost = true;
        }
        if (const auto details = _usage->find("prompt_tokens_details");
            details != _usage->end() && details->is_object()) {
            if (auto value = get_uint64(*details, "cached_tokens")) {
                cost.cache_hit = *value;
                has_cost = true;
            }
        }
        if (has_cost) result.cost = cost;
    }

    nlohmann::json extras = nlohmann::json::object();
    if (!_completion_id.empty()) extras["completion_id"] = _completion_id;
    if (!_model.empty()) extras["model"] = _model;
    if (!_finish_reason.empty()) extras["finish_reason"] = _finish_reason;
    if (_usage) extras["usage"] = *_usage;
    if (_error) extras["error"] = *_error;
    if (!extras.empty()) result.extras = std::move(extras);
    _response = std::move(result);
}

} // namespace llm::chat_completions
