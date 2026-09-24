#include "io/client.hpp"

#include <stdexcept>
#include <utility>

#include "logging/logger.hpp"

namespace io {

Client::Client(boost::asio::any_io_executor executor,
               endpoint::ResolvedEndpoint endpoint,
               eventbus::EventBus& events,
               ClientOptions options,
               intercom::StableWebSocketOptions transport_options,
               endpoint::ssl_context& tls_context)
    : StableWebSocketClient(executor, std::move(endpoint), transport_options,
                            tls_context),
      _state(std::make_shared<State>(executor, _signal_io.get_executor(),
                                     options)),
      _signal_done(executor, 1),
      _signal_handler([&events](const nlohmann::json& signal) {
          events.publish(SignalEvent{signal});
      })
{
    if (options.payload_capacity == 0 || options.signal_capacity == 0) {
        throw std::invalid_argument("IO channel capacities must be positive");
    }
}

Client::~Client() {
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
        throw std::logic_error("io::Client::run may only be called once");
    }

    std::exception_ptr transport_failure;
    const bool start_worker = !_state->stopping.load();
    if (start_worker) {
        boost::asio::co_spawn(_signal_io, process_signals(),
            [this](std::exception_ptr error) {
                if (error) {
                    _signal_failure = error;
                    StableWebSocketClient::stop();
                }
                _signal_done.try_send(boost::system::error_code{});
            });
        _signal_thread = std::jthread([this] { _signal_io.run(); });
    }

    try {
        co_await StableWebSocketClient::run(stop);
    } catch (...) {
        transport_failure = std::current_exception();
    }

    close_queues();
    if (start_worker) {
        // The signal handler may still be running synchronously. Suspending
        // here leaves the network executor free to run timers and payload work
        // that the handler itself may be waiting for.
        co_await _signal_done.async_receive(boost::asio::use_awaitable);
        _signal_thread.join();
    }

    if (_signal_failure) std::rethrow_exception(_signal_failure);
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

void Client::on_text(std::string message) {
    if (_state->stopping.load()) return;
    nlohmann::json envelope = nlohmann::json::parse(message);
    if (!envelope.is_object() || !envelope.contains("type") ||
        !envelope.at("type").is_string() || !envelope.contains("data")) {
        throw std::invalid_argument("invalid IO message envelope");
    }

    const std::string type = envelope.at("type").get<std::string>();
    nlohmann::json data = std::move(envelope["data"]);
    if (type == "payload") {
        if (!_state->payloads.try_send(boost::system::error_code{},
                                       std::move(data))) {
            if (_state->stopping.load()) return;
            _state->rejected_payloads.fetch_add(1);
            logging::Logger::warning(
                "io client: rejected payload because the queue is full");
        }
    } else if (type == "signal") {
        if (!_state->signals.try_send(boost::system::error_code{},
                                      std::move(data))) {
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
        nlohmann::json signal = co_await _state->signals.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (_state->stopping.load() || ec) co_return;

        SignalHandler handler;
        {
            std::lock_guard lock(_handler_mutex);
            handler = _signal_handler;
        }
        try {
            handler(signal);
        } catch (...) {
            _signal_failure = std::current_exception();
            StableWebSocketClient::stop();
            co_return;
        }
    }
}

void Client::close_queues() noexcept {
    if (_state->stopping.exchange(true)) return;
    _state->payloads.close();
    _state->signals.close();
}

} // namespace io
