#include "llm/chat_completions/stream_handler.hpp"

#include <string>
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

std::optional<std::size_t> get_index(
    const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return std::nullopt;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_unsigned()) return std::nullopt;
    return it->get<std::size_t>();
}

const nlohmann::json* find_object(
    const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return nullptr;
    const auto it = object.find(key);
    return it != object.end() && it->is_object() ? &*it : nullptr;
}

ChatCompletionsDelta ignored(nlohmann::json extras = {}) {
    ChatCompletionsDelta result;
    result.ignored = true;
    if (!extras.is_null()) result.extras = std::move(extras);
    return result;
}

} // namespace

ChatCompletionsDelta ChatCompletionsStreamHandler::_handle_message(
    std::span<const LineInfo> message) {
    std::string data;
    bool has_data = false;
    for (const auto& [field, value] : message) {
        if (field != "data") continue;
        if (has_data) data += '\n';
        data += value;
        has_data = true;
    }
    if (!has_data) return ignored();

    if (data == "[DONE]") {
        ChatCompletionsDelta result;
        result.done = true;
        return result;
    }

    nlohmann::json chunk = nlohmann::json::parse(data, nullptr, false);
    if (chunk.is_discarded()) return ignored(data);
    try {
        chunk = _dialect->normalize_chunk(std::move(chunk));
    } catch (...) {
        return ignored(nlohmann::json::parse(data, nullptr, false));
    }
    if (!chunk.is_object()) return ignored(std::move(chunk));

    ChatCompletionsDelta result;
    result.extras = chunk;
    if (find_object(chunk, "error")) {
        result.error = true;
        return result;
    }
    if (const nlohmann::json* usage = find_object(chunk, "usage")) {
        result.usage = *usage;
    }

    const auto choices = chunk.find("choices");
    if (choices == chunk.end() || !choices->is_array() || choices->empty()) {
        // A usage-only final chunk is meaningful even without a choice.
        result.ignored = !result.usage.has_value();
        return result;
    }

    // `n` is builder-owned and fixed at one. Still select choice index zero
    // explicitly so a proxy that reorders a malformed multi-choice chunk is
    // handled deterministically.
    const nlohmann::json* choice = nullptr;
    for (const auto& candidate : *choices) {
        if (get_index(candidate, "index").value_or(0) == 0) {
            choice = &candidate;
            break;
        }
    }
    if (!choice || !choice->is_object()) {
        result.ignored = true;
        return result;
    }
    result.choice_index = get_index(*choice, "index").value_or(0);
    result.finish_reason = get_string(*choice, "finish_reason");

    const nlohmann::json* delta = find_object(*choice, "delta");
    if (!delta) return result;
    result.role = get_string(*delta, "role");
    result.content = get_string(*delta, "content");
    result.reasoning = get_string(*delta, "reasoning_content");
    result.refusal = get_string(*delta, "refusal");

    const auto calls = delta->find("tool_calls");
    if (calls != delta->end() && calls->is_array()) {
        for (const auto& wire_call : *calls) {
            if (!wire_call.is_object()) continue;
            ToolCallDelta call;
            call.index = get_index(wire_call, "index").value_or(0);
            call.id = get_string(wire_call, "id");
            call.type = get_string(wire_call, "type");
            if (const nlohmann::json* function =
                    find_object(wire_call, "function")) {
                call.name = get_string(*function, "name");
                call.arguments = get_string(*function, "arguments");
            }
            result.tool_calls.push_back(std::move(call));
        }
    }
    return result;
}

} // namespace llm::chat_completions
