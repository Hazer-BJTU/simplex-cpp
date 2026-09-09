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

/**
 * Provider-neutral, streaming Chat Completions implementation of LLMModel.
 *
 * Concurrency: converse() and provider_info() are REENTRANT — any number may
 * be in flight on one instance, on any threads of the executor. Everything
 * per-exchange (interpreter, reader, retry engine) is constructed inside the
 * call, the mutable generation knobs are read once as a snapshot, and the
 * members read afterwards (_endpoint, _dialect, the retry policy) are
 * immutable after build(). Concurrent exchanges are told apart by the
 * exchange id on their events and on the returned MessageItem's extras (see
 * llm/chat_completions/events.hpp). The caller keeps the model alive for the
 * duration of each exchange and folds concurrent results into separate
 * AgentInputState objects — see the LLMModel class doc for the full contract.
 */
class ChatCompletionsModel : public llm::LLMModel {
public:
    LLMModelType model_type() const noexcept final {
        return LLMModelType::Conversation;
    }

    bool build() noexcept override;

    /// One reentrant exchange; @p conversation by value (the base contract
    /// explains why a coroutine must own its copy).
    boost::asio::awaitable<model_io::MessageItem> converse(
        model_io::AgentInputState conversation) override;

    boost::asio::awaitable<nlohmann::json> provider_info() override;

    const model_io::ModelEndpoint& endpoint() const noexcept { return _endpoint; }

protected:
    ChatCompletionsModel(
        boost::asio::any_io_executor executor,
        nlohmann::json config,
        ChatCompletionsDialectPtr dialect = default_dialect())
        : LLMModel(std::move(executor), std::move(config)),
          _dialect(dialect ? std::move(dialect) : default_dialect()) {}

private:
    model_io::ModelEndpoint _endpoint;
    ChatCompletionsDialectPtr _dialect;
    std::chrono::milliseconds _initial_backoff{500};
    std::chrono::milliseconds _max_backoff{120000};
    unsigned _max_retry_attempts = 3;
    bool _built = false;
};

} // namespace llm::chat_completions
