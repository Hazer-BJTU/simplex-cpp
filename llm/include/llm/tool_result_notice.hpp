#pragma once

#include "dataclass/model_io.hpp"

namespace llm {

/// Private request-snapshot metadata supplied by the worker. A provider can
/// append the text to the identified tool result after preparing its wire body.
inline constexpr char tool_result_notice_key[] = "simplex.tool_result_notice";

enum class ToolResultFormat { ChatCompletions, Responses };

/// Append only to the matching final wire item. Captured Responses outputs
/// and dialect patches have already been applied. A dialect that removes or
/// replaces this result must never redirect the notice onto another message.
inline void append_tool_result_notice(const model_io::AgentInputState& snapshot,
    nlohmann::json& body, ToolResultFormat format) {
    if (!snapshot.extras || !snapshot.extras->contains(tool_result_notice_key)) return;
    const auto& notice = snapshot.extras->at(tool_result_notice_key);
    const bool chat = format == ToolResultFormat::ChatCompletions;
    auto items = body.find(chat ? "messages" : "input");
    if (items == body.end() || !items->is_array() || items->empty()) return;

    auto& last = items->back();
    if (!last.is_object() || !notice.is_object()) return;
    const auto kind = last.find(chat ? "role" : "type");
    const auto id = last.find(chat ? "tool_call_id" : "call_id");
    const auto text = notice.find("text");
    if (kind == last.end() || *kind != (chat ? "tool" : "function_call_output")
        || id == last.end() || !id->is_string() || !notice.contains("call_id")
        || *id != notice.at("call_id") || text == notice.end() || !text->is_string()) return;

    auto output = last.find(chat ? "content" : "output");
    if (output == last.end()) return;
    if (output->is_string()) {
        auto& value = output->get_ref<std::string&>();
        if (!value.empty()) value += "\n\n";
        value += text->get_ref<const std::string&>();
    } else if (output->is_array()) {
        output->push_back({{"type", chat ? "text" : "input_text"}, {"text", *text}});
    }
}

} // namespace llm
