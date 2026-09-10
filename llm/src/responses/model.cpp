#include "llm/responses/model.hpp"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include "endpoint/complete.hpp"
#include "endpoint/request.hpp"
#include "llm/exchange_id.hpp"
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

        // Derive locally, validate, publish once through the guarded setter —
        // the base class keeps _generation private so no in-flight exchange
        // can read a partly derived object.
        json generation = _config;
        generation.erase("endpoint");
        generation.erase("provider");
        generation.erase("retry");

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

        const auto model = generation.find("model");
        if (model == generation.end() || !model->is_string() ||
            model->get_ref<const std::string&>().empty()) {
            return false;
        }
        reset_generation(std::move(generation));
        _built = true;
        return true;
    } catch (...) {
        return false;
    }
}

boost::asio::awaitable<model_io::MessageItem> ResponsesModel::converse(
    model_io::AgentInputState conversation) {
    if (!_built) {
        throw std::logic_error("ResponsesModel used before successful build()");
    }

    // ---- the reentrancy prologue: everything shared, read ONCE, up front ----
    // Read before the first suspension and never again, so a concurrent
    // exchange (or a concurrent set_generation()) can neither tear these
    // values nor be torn by them. The generation knobs are the one mutable
    // member, hence the snapshot; the rest are immutable after build() and
    // are copied only so nothing below depends on `this`.
    const json generation = generation_snapshot();
    const ResponsesDialectPtr dialect = _dialect;
    const endpoint::ResolvedEndpoint where = endpoint::resolve_endpoint(_endpoint);
    const std::string exchange_id = llm::next_exchange_id();

    ResponsesInterpreter interpreter(dialect);
    auto request = interpreter.build_request(conversation, _endpoint, generation);
    auto reader = std::make_shared<ResponsesReader>(_executor, dialect);

    // Per-call retry engine. MUST stay a local: endpoint::complete keeps
    // rolling backoff state and documents one operator() in flight per
    // instance, so hoisting this into a member would break reentrancy.
    endpoint::complete<ResponsesDelta> exchange(
        _executor, _initial_backoff, _max_backoff, _max_retry_attempts);
    auto result = co_await exchange(
        where, std::move(request), reader,
        endpoint::sse_request<ResponsesDelta>);

    const ResponseStatus status = reader->response_status();
    if (status != ResponseStatus::Completed) {
        nlohmann::json details = reader->terminal_details().value_or(
            nlohmann::json::object());
        throw ResponsesApiException(
            status, details, terminal_message(status, details));
    }
    // The correlation id every adapter reports, so hosts identify an
    // exchange's result the same way whichever protocol produced it.
    if (!result.extras) {
        result.extras = json::object();
    }
    if (result.extras->is_object()) {
        (*result.extras)["exchange_id"] = exchange_id;
    }
    co_return result;
}

boost::asio::awaitable<nlohmann::json> ResponsesModel::provider_info() {
    if (!_built) {
        throw std::logic_error("ResponsesModel used before successful build()");
    }
    // Own copies before the first suspension: fetch_provider_models holds the
    // endpoint BY REFERENCE across its co_awaits, so a frame-local copy keeps
    // the exchange independent of the model's lifetime margin.
    const model_io::ModelEndpoint endpoint = _endpoint;
    const ResponsesDialectPtr dialect = _dialect;
    co_return co_await llm::fetch_provider_models(
        _executor, endpoint, dialect->models_path());
}

} // namespace llm::responses
