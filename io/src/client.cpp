#include "io/client.hpp"

#include <stdexcept>
#include <utility>

#include "logging/logger.hpp"

namespace io {

namespace {

ClientOptions validate_options(ClientOptions options) {
    if (options.payload_capacity == 0 || options.signal_capacity == 0 || options.query_capacity == 0) {
        throw std::invalid_argument("IO channel capacities must be positive");
    }
    return options;
}

} // namespace

Client::Client(boost::asio::any_io_executor executor,
               endpoint::ResolvedEndpoint endpoint,
               eventbus::EventBus& events,
               ClientOptions options,
               intercom::StableWebSocketOptions transport_options,
               endpoint::ssl_context& tls_context)
    : StableWebSocketClient(executor, std::move(endpoint), transport_options,
                            tls_context),
      _executor(executor),
      _state(std::make_shared<State>(executor, _signal_io.get_executor(),
                                     validate_options(options))),
      _signal_done(executor, 1),
      _messages_done(executor, 2),
      _events(events),
      _signal_handler([&events](const nlohmann::json& signal) {
          events.publish(SignalEvent{signal});
      })
{}

Client::~Client() {
    // Best-effort signal-worker cleanup only. Destruction while run() is
    // active is invalid: run() is the transport completion fence.
    stop();
    if (_signal_thread.joinable()) {
        _signal_thread.join();
    }
}

boost::asio::awaitable<void> Client::send(nlohmann::json message) {
    return StableWebSocketClient::send(message.dump());
}

boost::asio::awaitable<void> Client::run(std::stop_token stop) {
    if (_started.exchange(true)) {
        throw std::logic_error("io::Client run() may only be called once");
    }

    std::exception_ptr transport_failure;
    const bool start_worker = !_state->stopping.load();
    if (start_worker) {
        boost::asio::co_spawn(_signal_io, process_signals(),
            [this](std::exception_ptr error) {
                if (error) {
                    StableWebSocketClient::stop();
                }
                _signal_done.try_send(boost::system::error_code{}, error);
            });
        _signal_thread = std::jthread([this] { _signal_io.run(); });
        auto completion = [this](std::exception_ptr error) {
            if (error) StableWebSocketClient::stop();
            _messages_done.try_send(boost::system::error_code{}, error);
        };
        boost::asio::co_spawn(_executor, process_queries(), completion);
        boost::asio::co_spawn(_executor, process_feedback(), completion);
    }

    try {
        co_await StableWebSocketClient::run(stop);
    } catch (...) {
        transport_failure = std::current_exception();
    }

    close_queues();
    std::exception_ptr signal_failure;
    if (start_worker) {
        // The signal handler may still be running synchronously. Suspending
        // here leaves the network executor free to run timers and payload work
        // that the handler itself may be waiting for.
        signal_failure = co_await _signal_done.async_receive(
            boost::asio::use_awaitable);
        _signal_thread.join();
        for (int worker = 0; worker < 2; ++worker) {
            auto error = co_await _messages_done.async_receive(boost::asio::use_awaitable);
            if (!signal_failure) signal_failure = error;
        }
    }

    if (signal_failure) std::rethrow_exception(signal_failure);
    if (transport_failure) std::rethrow_exception(transport_failure);
}

void Client::stop() {
    close_queues();
    StableWebSocketClient::stop();
}

Client::PayloadSubscription Client::subscribe_payload() {
    if (_state->subscribed.exchange(true)) {
        throw std::logic_error("payload queue already has a subscriber");
    }
    if (_state->stopping.load()) {
        throw std::logic_error("cannot subscribe to a stopped client");
    }
    return PayloadSubscription(_state);
}

boost::asio::awaitable<nlohmann::json>
Client::PayloadSubscription::next() {
    auto state = _state;
    if (!state) {
        throw std::logic_error("payload subscription has been moved from");
    }
    if (state->receiving.exchange(true)) {
        throw std::logic_error("payload subscription already has a pending next()");
    }
    struct ResetPending {
        std::shared_ptr<State> state;
        ~ResetPending() { state->receiving.store(false); }
    } reset{state};
    co_return co_await state->payloads.async_receive(boost::asio::use_awaitable);
}

void Client::register_signal_handler(SignalHandler handler) {
    if (!handler) {
        throw std::invalid_argument("signal handler must be callable");
    }
    std::lock_guard lock(_handler_mutex);
    _signal_handler = std::move(handler);
}

std::size_t Client::rejected_payloads() const noexcept {
    return _state->rejected_payloads.load();
}

std::size_t Client::rejected_queries() const noexcept {
    return _state->rejected_queries.load();
}

std::size_t Client::unreported_rejections() const noexcept {
    return _state->unreported_rejections.load();
}

void Client::queue_feedback(nlohmann::json metadata) {
    // Malformed correlation values must not turn a metadata-only rejection
    // mailbox into a retained copy of an oversized rejected input.
    if (metadata.at("request_id").dump().size() > 512) metadata["request_id"] = nullptr;
    if (metadata.contains("operation") && metadata.at("operation").dump().size() > 128)
        metadata["operation"] = nullptr;
    if (!_state->feedback.try_send(boost::system::error_code{}, std::move(metadata))) {
        if (!_state->stopping.load()) _state->unreported_rejections.fetch_add(1);
    }
}

void Client::on_text(std::string message) {
    if (_state->stopping.load()) return;
    nlohmann::json envelope = nlohmann::json::parse(message);
    if (!envelope.is_object() || !envelope.contains("type") ||
        !envelope.at("type").is_string() || !envelope.contains("data")) {
        throw std::invalid_argument("invalid IO message envelope");
    }

    const std::string type = envelope.at("type").get<std::string>();
    nlohmann::json data = std::move(envelope["data"]);
    if (type == "payload" && data.is_object()
        && (data.value("operation", nlohmann::json()) == "history"
            || data.value("operation", nlohmann::json()) == "answer")) {
        // Query bodies are small protocol cursors, not conversation input.
        // Reject large queries before filling the independent count quota.
        const auto operation = data.at("operation").get<std::string>();
        auto id = data.value("request_id", nlohmann::json());
        if (!id.is_string() || id.get_ref<const std::string&>().size() > 128) id = nullptr;
        const bool oversized = message.size() > 16 * 1024;
        if (oversized || !_state->queries.try_send(boost::system::error_code{}, std::move(data))) {
            if (_state->stopping.load()) return;
            _state->rejected_queries.fetch_add(1);
            queue_feedback({{"type", "query_rejected"}, {"request_id", std::move(id)},
                {"operation", operation}, {"code", oversized ? "query_too_large" : "query_queue_full"}});
        }
    } else if (type == "payload") {
        // Save only correlation metadata before moving the payload into the
        // channel. Never retain rejected content or assume a failed channel
        // send leaves its rvalue argument untouched.
        nlohmann::json rejection = {
            {"type", "payload_rejected"},
            {"request_id", data.is_object()
                ? data.value("request_id", nlohmann::json()) : nlohmann::json()},
            {"operation", data.is_object() && data.contains("operation")
                && data.at("operation").is_string()
                ? data.at("operation") : nlohmann::json()}
        };
        if (!_state->payloads.try_send(boost::system::error_code{},
                                       std::move(data))) {
            if (_state->stopping.load()) return;
            _state->rejected_payloads.fetch_add(1);
            logging::Logger::warning(
                "io client: rejected payload because the queue is full");
            queue_feedback(std::move(rejection));
        }
    } else if (type == "signal") {
        if (!_state->signals.try_send(boost::system::error_code{},
                nlohmann::json{{"type", "signal"}, {"data", std::move(data)}})) {
            if (_state->stopping.load()) return;
            throw std::runtime_error("IO signal queue is full");
        }
    } else {
        throw std::invalid_argument("unknown IO message type: " + type);
    }
}

boost::asio::awaitable<void> Client::process_signals() {
    for (;;) {
        boost::system::error_code ec;
        nlohmann::json control = co_await _state->signals.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (_state->stopping.load() || ec) co_return;

        SignalHandler handler;
        {
            std::lock_guard lock(_handler_mutex);
            handler = _signal_handler;
        }
        handler(control.at("data"));
    }
}

/** Queries run on the caller's executor, independently of the control thread. */
boost::asio::awaitable<void> Client::process_queries() {
    for (;;) {
        boost::system::error_code error;
        auto payload = co_await _state->queries.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || _state->stopping.load()) co_return;
        _events.publish(PayloadQueryEvent{std::move(payload)});
    }
}

/** Feedback has its own bounded mailbox; rejection never recursively queues rejection. */
boost::asio::awaitable<void> Client::process_feedback() {
    for (;;) {
        boost::system::error_code error;
        auto metadata = co_await _state->feedback.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, error));
        if (error || _state->stopping.load()) co_return;
        if (metadata.at("type") == "query_rejected") {
            _events.publish(PayloadQueryRejectedEvent{
                std::move(metadata["request_id"]), metadata.at("operation").get<std::string>(),
                metadata.at("code").get<std::string>()});
        } else {
            _events.publish(PayloadRejectedEvent{
                std::move(metadata["request_id"]), std::move(metadata["operation"])});
        }
    }
}

void Client::close_queues() noexcept {
    if (_state->stopping.exchange(true)) return;
    _state->payloads.close();
    _state->queries.close();
    _state->feedback.close();
    _state->signals.close();
}

} // namespace io
