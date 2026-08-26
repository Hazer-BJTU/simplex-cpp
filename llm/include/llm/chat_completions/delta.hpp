#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace llm::chat_completions {

struct ToolCallDelta {
    std::size_t index = 0;
    std::string id;
    std::string type;
    std::string name;
    std::string arguments;
};

/** One decoded Chat Completions SSE frame (one frame may update many calls). */
struct ChatCompletionsDelta {
    std::size_t choice_index = 0;
    std::string role;
    std::string content;
    std::string refusal;
    std::vector<ToolCallDelta> tool_calls;
    std::string finish_reason;
    std::optional<nlohmann::json> usage;
    std::optional<nlohmann::json> extras;
    bool done = false;
    bool error = false;
    bool ignored = false;
};

inline bool is_terminal(const ChatCompletionsDelta& delta) {
    return delta.done || delta.error;
}

} // namespace llm::chat_completions
