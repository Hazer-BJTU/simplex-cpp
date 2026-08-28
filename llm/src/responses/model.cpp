#include "llm/responses/model.hpp"

#include <cstdint>
#include <memory>
#include <ostream>
#include <utility>

#include "endpoint/complete.hpp"
#include "endpoint/request.hpp"
#include "llm/provider_models.hpp"
#include "llm/responses/interpreter.hpp"
#include "llm/responses/reader.hpp"

namespace llm::responses {

namespace {

using nlohmann::json;

void overlay_object(json& target, const json& overlay) {
    if (!overlay.is_object()) return;
    if (!target.is_object()) target = json::object();
    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        if (it.value().is_object() && target.contains(it.key()) &&
            target[it.key()].is_object()) {
            overlay_object(target[it.key()], it.value());
        } else if (!it.value().is_null()) {
            target[it.key()] = it.value();
        }
    }
}

// Positive-integer reader for the retry object. is_number_integer() (not
// is_number_unsigned()): positive integers arrive as number_integer from
// both C++ int literals and the YAML bridge's int64-first conversion, so
// the unsigned-only check would silently ignore every hand-written config.
std::chrono::milliseconds read_milliseconds(
    const json& retry, const char* key, std::chrono::milliseconds fallback) {
    const auto it = retry.find(key);
    if (it == retry.end() || !it->is_number_integer()) return fallback;
    const std::int64_t value = it->get<std::int64_t>();
    if (value < 0) return fallback;
    return std::chrono::milliseconds(value);
}

std::string terminal_message(ResponseStatus status, const json& details) {
    if (details.is_object()) {
        const auto message = details.find("message");
        if (message != details.end() && message->is_string()) {
            return message->get<std::string>();
        }
        const auto error = details.find("error");
        if (error != details.end() && error->is_object()) {
            const auto nested = error->find("message");
            if (nested != error->end() && nested->is_string()) {
                return nested->get<std::string>();
            }
        }
    }
    switch (status) {
        case ResponseStatus::Incomplete: return "Responses API response is incomplete";
        case ResponseStatus::Failed: return "Responses API response failed";
        case ResponseStatus::Errored: return "Responses API stream reported an error";
        case ResponseStatus::Cancelled: return "Responses API response was cancelled";
        case ResponseStatus::Aborted: return "Responses API stream was aborted";
        default: return "Responses API response did not complete";
    }
}

} // namespace

ResponsesApiException::ResponsesApiException(
    ResponseStatus status, json details, std::string message)
    : std::runtime_error(std::move(message)),
      _status(status),
      _details(std::move(details)) {}

std::ostream& operator<<(std::ostream& os, ResponseStatus status) {
    switch (status) {
        case ResponseStatus::Streaming: return os << "Streaming";
        case ResponseStatus::Completed: return os << "Completed";
        case ResponseStatus::Incomplete: return os << "Incomplete";
        case ResponseStatus::Failed: return os << "Failed";
        case ResponseStatus::Errored: return os << "Errored";
        case ResponseStatus::Cancelled: return os << "Cancelled";
        case ResponseStatus::Aborted: return os << "Aborted";
    }
    return os << "ResponseStatus(?)";
}

bool ResponsesModel::build() noexcept {
    if (_built) return true;
    try {
        if (!_config.is_object()) return false;

        json endpoint_json = _dialect->default_endpoint();
        if (const auto it = _config.find("endpoint");
            it != _config.end() && it->is_object()) {
            overlay_object(endpoint_json, *it);
        }
        _endpoint = endpoint_json.get<model_io::ModelEndpoint>();
        (void)endpoint::resolve_endpoint(_endpoint);

        _generation = _config;
        _generation.erase("endpoint");
        _generation.erase("provider");
        _generation.erase("retry");

        if (const auto retry = _config.find("retry");
            retry != _config.end() && retry->is_object()) {
            _initial_backoff = read_milliseconds(
                *retry, "initial_backoff_ms", _initial_backoff);
            _max_backoff = read_milliseconds(
                *retry, "max_backoff_ms", _max_backoff);
            const auto attempts = retry->find("max_attempts");
            if (attempts != retry->end() && attempts->is_number_integer()) {
                // Retries only — the initial exchange is not counted (the
                // endpoint::complete budget contract); 0 disables retrying.
                const std::int64_t value = attempts->get<std::int64_t>();
                _max_retry_attempts =
                    value > 0 ? static_cast<unsigned>(value) : 0u;
            }
        }

        const auto model = _generation.find("model");
        if (model == _generation.end() || !model->is_string() ||
            model->get_ref<const std::string&>().empty()) {
            return false;
        }
        _built = true;
        return true;
    } catch (...) {
        return false;
    }
}

boost::asio::awaitable<model_io::MessageItem> ResponsesModel::converse(
    const model_io::AgentInputState& conversation) {
    if (!_built) {
        throw std::logic_error("ResponsesModel used before successful build()");
    }

    ResponsesInterpreter interpreter(_dialect);
    auto request = interpreter.build_request(conversation, _endpoint, _generation);
    auto reader = std::make_shared<ResponsesReader>(_executor, _dialect);

    endpoint::complete<ResponsesDelta> exchange(
        _executor, _initial_backoff, _max_backoff, _max_retry_attempts);
    auto result = co_await exchange(
        endpoint::resolve_endpoint(_endpoint), std::move(request), reader,
        endpoint::sse_request<ResponsesDelta>);

    const ResponseStatus status = reader->response_status();
    if (status != ResponseStatus::Completed) {
        nlohmann::json details = reader->terminal_details().value_or(
            nlohmann::json::object());
        throw ResponsesApiException(
            status, details, terminal_message(status, details));
    }
    co_return result;
}

boost::asio::awaitable<nlohmann::json> ResponsesModel::provider_info() {
    if (!_built) {
        throw std::logic_error("ResponsesModel used before successful build()");
    }
    co_return co_await llm::fetch_provider_models(
        _executor, _endpoint, _dialect->models_path());
}

} // namespace llm::responses
