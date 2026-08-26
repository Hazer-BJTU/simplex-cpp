#pragma once

#include <span>
#include <utility>

#include "endpoint/request.hpp"
#include "llm/chat_completions/delta.hpp"
#include "llm/chat_completions/dialect.hpp"

namespace llm::chat_completions {

class ChatCompletionsStreamHandler final
    : public endpoint::SSEResponseHandler<ChatCompletionsDelta> {
public:
    explicit ChatCompletionsStreamHandler(
        boost::asio::any_io_executor executor,
        std::size_t line_window = endpoint::DEFAULT_SSE_LINE_WINDOW,
        ChatCompletionsDialectPtr dialect = default_dialect())
        : endpoint::SSEResponseHandler<ChatCompletionsDelta>(
              std::move(executor), line_window),
          _dialect(dialect ? std::move(dialect) : default_dialect()) {}

protected:
    ChatCompletionsDelta _handle_message(
        std::span<const LineInfo> message) override;

private:
    ChatCompletionsDialectPtr _dialect;
};

} // namespace llm::chat_completions
