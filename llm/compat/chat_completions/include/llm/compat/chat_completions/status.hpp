#pragma once

#include <iosfwd>

namespace llm::chat_completions {

enum class ChatCompletionStatus {
    Streaming,
    Completed,
    LengthLimited,
    ContentFiltered,
    Failed,
    Aborted,
};

std::ostream& operator<<(std::ostream& os, ChatCompletionStatus status);

} // namespace llm::chat_completions
