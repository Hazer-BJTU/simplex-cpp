#pragma once

#include <chrono>
#include <stdexcept>
#include <string>

#include <boost/asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"
#include "llm/models.hpp"
#include "llm/responses/dialect.hpp"
#include "llm/responses/status.hpp"

namespace llm::responses {

class ResponsesApiException : public std::runtime_error {
public:
    ResponsesApiException(ResponseStatus status, nlohmann::json details,
                          std::string message);

    ResponseStatus status() const noexcept { return _status; }
    const nlohmann::json& details() const noexcept { return _details; }

private:
    ResponseStatus _status;
    nlohmann::json _details;
};

/**
 * A usable canonical Responses-API implementation of LLMModel.
 *
 * converse() runs one endpoint::complete exchange: the interpreter flattens
 * the AgentInputState (+ the stored generation config) into a POST /responses
 * body, the stream handler decodes the SSE events (dialect-normalised), and
 * the reader accumulates them into the returned MessageItem. The whole
 * retry/backoff policy comes from the config's "retry" object (see below);
 * a stream that does not end in ResponseStatus::Completed surfaces as
 * ResponsesApiException (details = the terminal event's response record).
 *
 * Config shape (the "endpoint" value is ModelEndpoint's serialised form,
 * recursively overlaid on the dialect's default endpoint; "provider",
 * "endpoint" and "retry" are host-only keys stripped from the generation
 * object passed to the interpreter):
 *
 *   {
 *     "model": "provider-model-name",        // required, non-empty
 *     "endpoint": { "base_url": "...", ... },
 *     "retry": {
 *       "initial_backoff_ms": 500,            // default 500
 *       "max_backoff_ms": 120000,             // default 120000
 *       "max_attempts": 3                     // RETRIES after the initial
 *     }                                       // exchange; 0 disables
 *   }
 *
 * Everything else passes through to the request body verbatim.
 */
class ResponsesModel : public llm::LLMModel {
public:
    LLMModelType model_type() const noexcept final {
        return LLMModelType::Conversation;
    }

    bool build() noexcept override;

    boost::asio::awaitable<model_io::MessageItem> converse(
        const model_io::AgentInputState& conversation) override;

    boost::asio::awaitable<nlohmann::json> provider_info() override;

    const model_io::ModelEndpoint& endpoint() const noexcept { return _endpoint; }

protected:
    ResponsesModel(boost::asio::any_io_executor executor, nlohmann::json config,
                   ResponsesDialectPtr dialect = default_dialect())
        : LLMModel(std::move(executor), std::move(config)),
          _dialect(dialect ? std::move(dialect) : default_dialect()) {}

private:
    model_io::ModelEndpoint _endpoint;
    ResponsesDialectPtr _dialect;
    std::chrono::milliseconds _initial_backoff{500};
    std::chrono::milliseconds _max_backoff{120000};
    unsigned _max_retry_attempts = 3;
    bool _built = false;
};

} // namespace llm::responses
