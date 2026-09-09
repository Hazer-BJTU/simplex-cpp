#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "dataclass/model_io.hpp"
#include "endpoint/model_request.hpp"
#include "llm/chat_completions/delta.hpp"
#include "llm/chat_completions/dialect.hpp"
#include "llm/chat_completions/status.hpp"
#include "llm/chat_completions/stream_handler.hpp"

namespace llm::chat_completions {

class ChatCompletionsReader final
    : public endpoint::ModelResponseReader<ChatCompletionsDelta> {
public:
    explicit ChatCompletionsReader(
        boost::asio::any_io_executor executor,
        std::size_t line_window = endpoint::DEFAULT_SSE_LINE_WINDOW,
        ChatCompletionsDialectPtr dialect = default_dialect())
        : ModelResponseReader(
              std::make_shared<ChatCompletionsStreamHandler>(
                  std::move(executor), line_window, std::move(dialect))) {}

    explicit ChatCompletionsReader(
        boost::asio::any_io_executor executor,
        ChatCompletionsDialectPtr dialect,
        std::size_t line_window = endpoint::DEFAULT_SSE_LINE_WINDOW)
        : ChatCompletionsReader(
              std::move(executor), line_window, std::move(dialect)) {}

    explicit ChatCompletionsReader(
        std::shared_ptr<ChatCompletionsStreamHandler> handler)
        : ModelResponseReader(std::move(handler)) {}

    const model_io::MessageItem& response() const noexcept override {
        return _response;
    }

    ChatCompletionStatus status() const noexcept {
        if (_status == ChatCompletionStatus::Streaming && finished()) {
            return ChatCompletionStatus::Aborted;
        }
        return _status;
    }

    const std::optional<nlohmann::json>& error_details() const noexcept {
        return _error;
    }

    /**
     * @brief Which transport attempt this reader is currently accumulating:
     *        0 for the initial exchange, 1.. for each retry.
     *
     * endpoint::complete calls clear() before EVERY attempt (the first
     * included), so the counter is simply "clears seen so far", and a reader
     * that has never been cleared reports 0 like the initial attempt it is.
     * Read from a delta hook, it tells an observer whether the increments
     * arriving now are a fresh read or a replay of one it already saw — the
     * distinction a subscriber needs to reset its per-exchange buffer
     * instead of appending a duplicated prefix.
     */
    unsigned attempt() const noexcept { return _attempt; }

    void clear() override;

protected:
    void _accumulate(const ChatCompletionsDelta& delta) override;
    bool _is_terminal(const ChatCompletionsDelta& delta) const override {
        return is_terminal(delta);
    }
    void _assemble() override;

private:
    struct ToolCallState {
        std::string id;
        std::string type;
        std::string name;
        std::string arguments;
    };

    std::string _role = "assistant";
    std::string _content;
    std::string _reasoning;
    std::string _refusal;
    std::map<std::size_t, ToolCallState> _tool_calls;
    std::string _finish_reason;
    std::string _completion_id;
    std::string _model;
    std::optional<nlohmann::json> _usage;
    std::optional<nlohmann::json> _error;
    model_io::MessageItem _response;
    ChatCompletionStatus _status = ChatCompletionStatus::Streaming;
    /// Attempts seen: bumped by every clear() after the first. See attempt().
    unsigned _attempt = 0;
    /// Whether clear() has run at all — tells attempt 0's reset apart from a
    /// retry's, since endpoint::complete clears before the initial exchange too.
    bool _cleared_once = false;
};

} // namespace llm::chat_completions
