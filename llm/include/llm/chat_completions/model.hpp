#pragma once

#include <chrono>
#include <stdexcept>
#include <string>

#include "dataclass/endpoint_config.hpp"
#include "llm/chat_completions/dialect.hpp"
#include "llm/chat_completions/status.hpp"
#include "llm/models.hpp"

namespace llm::chat_completions {

class ChatCompletionsApiException : public std::runtime_error {
public:
    ChatCompletionsApiException(ChatCompletionStatus status,
                                nlohmann::json details,
                                std::string message);

    ChatCompletionStatus status() const noexcept { return _status; }
    const nlohmann::json& details() const noexcept { return _details; }

private:
    ChatCompletionStatus _status;
    nlohmann::json _details;
};

/** Provider-neutral, streaming Chat Completions implementation of LLMModel. */
class ChatCompletionsModel : public llm::LLMModel {
public:
    LLMModelType model_type() const noexcept final {
        return LLMModelType::Conversation;
    }

    bool build() noexcept override;

    boost::asio::awaitable<model_io::MessageItem> converse(
        const model_io::AgentInputState& conversation) override;

    const model_io::ModelEndpoint& endpoint() const noexcept { return _endpoint; }
    const nlohmann::json& generation() const noexcept { return _generation; }

protected:
    ChatCompletionsModel(
        boost::asio::any_io_executor executor,
        nlohmann::json config,
        ChatCompletionsDialectPtr dialect = default_dialect())
        : LLMModel(std::move(executor), std::move(config)),
          _dialect(dialect ? std::move(dialect) : default_dialect()) {}

private:
    model_io::ModelEndpoint _endpoint;
    nlohmann::json _generation = nlohmann::json::object();
    ChatCompletionsDialectPtr _dialect;
    std::chrono::milliseconds _initial_backoff{500};
    std::chrono::milliseconds _max_backoff{120000};
    unsigned _max_retry_attempts = 3;
    bool _built = false;
};

} // namespace llm::chat_completions
