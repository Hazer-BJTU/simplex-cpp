#include "llm/chat_completions/model.hpp"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include "endpoint/complete.hpp"
#include "endpoint/request.hpp"
#include "eventbus/event_bus.hpp"
#include "llm/chat_completions/events.hpp"
#include "llm/chat_completions/interpreter.hpp"
#include "llm/chat_completions/reader.hpp"
#include "llm/exchange_id.hpp"
#include "llm/provider_models.hpp"

namespace llm::chat_completions {

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

std::chrono::milliseconds read_milliseconds(
    const json& retry, const char* key, std::chrono::milliseconds fallback) {
    const auto it = retry.find(key);
    if (it == retry.end() || !it->is_number_integer()) return fallback;
    const std::int64_t value = it->get<std::int64_t>();
    return value < 0 ? fallback : std::chrono::milliseconds(value);
}

std::string failure_message(ChatCompletionStatus status, const json& details) {
    if (details.is_object()) {
        const auto message = details.find("message");
        if (message != details.end() && message->is_string()) {
            return message->get<std::string>();
        }
    }
    switch (status) {
        case ChatCompletionStatus::LengthLimited:
            return "Chat completion reached its token limit";
        case ChatCompletionStatus::ContentFiltered:
            return "Chat completion was content-filtered";
        case ChatCompletionStatus::Aborted:
            return "Chat completion stream was aborted";
        case ChatCompletionStatus::Failed:
            return "Chat completion failed";
        default:
            return "Chat completion did not complete";
    }
}

} // namespace

ChatCompletionsApiException::ChatCompletionsApiException(
    ChatCompletionStatus status, json details, std::string message)
    : std::runtime_error(std::move(message)),
      _status(status),
      _details(std::move(details)) {}

std::ostream& operator<<(std::ostream& os, ChatCompletionStatus status) {
    switch (status) {
        case ChatCompletionStatus::Streaming: return os << "Streaming";
        case ChatCompletionStatus::Completed: return os << "Completed";
        case ChatCompletionStatus::LengthLimited: return os << "LengthLimited";
        case ChatCompletionStatus::ContentFiltered: return os << "ContentFiltered";
        case ChatCompletionStatus::Failed: return os << "Failed";
        case ChatCompletionStatus::Aborted: return os << "Aborted";
    }
    return os << "ChatCompletionStatus(?)";
}

bool ChatCompletionsModel::build() noexcept {
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

        // Derive the knobs locally, validate, and publish once through
        // reset_generation() (the guarded setter) — the base class keeps
        // _generation private so no exchange can ever read a partly derived
        // object.
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
                const std::int64_t value = attempts->get<std::int64_t>();
                _max_retry_attempts = value > 0
                    ? static_cast<unsigned>(value)
                    : 0u;
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

boost::asio::awaitable<model_io::MessageItem> ChatCompletionsModel::converse(
    model_io::AgentInputState conversation) {
    if (!_built) {
        throw std::logic_error(
            "ChatCompletionsModel used before successful build()");
    }

    // ---- the reentrancy prologue: everything shared, read ONCE, up front ----
    // Every member touched here is read before the first suspension and never
    // again, so a concurrent exchange (or a concurrent set_generation()) can
    // neither tear these values nor be torn by them. The generation knobs are
    // the one mutable member, hence the snapshot rather than a direct read;
    // the rest (_endpoint, _dialect, the retry policy) are immutable after
    // build() and are copied only so nothing below depends on `this`.
    const json generation = generation_snapshot();
    const ChatCompletionsDialectPtr dialect = _dialect;
    const endpoint::ResolvedEndpoint where = endpoint::resolve_endpoint(_endpoint);

    ChatCompletionsInterpreter interpreter(dialect);
    auto request = interpreter.build_request(conversation, _endpoint, generation);
    auto reader = std::make_shared<ChatCompletionsReader>(_executor, dialect);

    // The default live-view hook: every streamed reasoning increment is
    // broadcast on the process-wide bus, synchronously, in wire order (see
    // llm/chat_completions/events.hpp for the full contract). The id is
    // minted once per exchange and is process-unique, so concurrent
    // exchanges — indistinguishable by provider/model, which are identical
    // across them — stay separable by it, and a retried attempt re-broadcasts
    // under the SAME id with the attempt counter advanced. The hook survives
    // the retry functor's reader clear(), and a bus with no subscriber is a
    // no-op — unobserved exchanges behave identically.
    const std::string exchange_id = llm::next_exchange_id();
    const std::string provider{dialect->provider_name()};
    std::string model;
    if (const auto it = generation.find("model");
        it != generation.end() && it->is_string()) {
        model = it->get_ref<const std::string&>();
    }
    // The reader is captured by raw pointer on purpose: it owns this hook, so
    // a shared_ptr here would be a cycle, and the hook only ever runs from
    // inside the reader's own next() — the pointer cannot be stale.
    reader->add_hook([exchange_id, provider, model, source = reader.get()](
                         const ChatCompletionsDelta& delta) {
        if (delta.reasoning.empty()) return;
        eventbus::default_bus().publish(ReasoningDeltaEvent{
            delta.reasoning, exchange_id, source->attempt(), provider, model});
    });

    // Per-call retry engine. MUST stay a local: endpoint::complete keeps
    // rolling backoff state and documents one operator() in flight per
    // instance, so hoisting this into a member would break reentrancy.
    endpoint::complete<ChatCompletionsDelta> exchange(
        _executor, _initial_backoff, _max_backoff, _max_retry_attempts);
    auto result = co_await exchange(
        where, std::move(request), reader,
        endpoint::sse_request<ChatCompletionsDelta>);

    const ChatCompletionStatus status = reader->status();
    if (status != ChatCompletionStatus::Completed) {
        json details = reader->error_details().value_or(json::object());
        throw ChatCompletionsApiException(
            status, details, failure_message(status, details));
    }
    // Report the correlation id back, so a subscriber that watched the live
    // stream can bind its buffer to the result the exchange produced.
    if (!result.extras) {
        result.extras = json::object();
    }
    if (result.extras->is_object()) {
        (*result.extras)["exchange_id"] = exchange_id;
    }
    co_return result;
}

boost::asio::awaitable<nlohmann::json> ChatCompletionsModel::provider_info() {
    if (!_built) {
        throw std::logic_error(
            "ChatCompletionsModel used before successful build()");
    }
    // Own copies before the first suspension, for the same reason converse()
    // takes its snapshot: fetch_provider_* hold the endpoint BY REFERENCE
    // across their co_awaits, so handing them a member would tie the
    // exchange's safety to the model outliving it by exactly the right
    // margin. A frame-local copy removes the question.
    const model_io::ModelEndpoint endpoint = _endpoint;
    const ChatCompletionsDialectPtr dialect = _dialect;

    nlohmann::json models = co_await llm::fetch_provider_models(
        _executor, endpoint, dialect->models_path());
    // A dialect exposing an account-balance companion (DeepSeek:
    // /user/balance) widens the return to one object — the models array
    // under "models", the provider's balance document verbatim under
    // "balance" — over a second single-shot GET on the same endpoint.
    if (std::string balance_path = dialect->balance_path();
        !balance_path.empty()) {
        nlohmann::json balance = co_await llm::fetch_provider_json(
            _executor, endpoint, std::move(balance_path));
        co_return nlohmann::json{
            {"models", std::move(models)},
            {"balance", std::move(balance)},
        };
    }
    co_return models;
}

} // namespace llm::chat_completions
