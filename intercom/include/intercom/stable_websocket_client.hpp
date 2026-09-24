#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/beast/core.hpp>

#include "endpoint/model_request.hpp"
#include "intercom/websocket_stream.hpp"
#include "logging/logger.hpp"

namespace intercom {

/** Configuration for a long-lived WebSocket client. */
struct StableWebSocketOptions {
    /// Maximum number of messages accepted while the writer is busy or offline.
    std::size_t write_capacity = 64;
    std::chrono::milliseconds initial_backoff{250};
    std::chrono::milliseconds max_backoff{10000};
    /// Disabled by default so an otherwise healthy silent session stays open.
    /// A positive value enables Beast's idle ping and peer-response deadline.
    std::chrono::seconds idle_timeout{0};
};

/**
 * @brief Maintain one WebSocket connection and dispatch complete text messages.
 *
 * run() is a single-use operation. The caller must keep this object, its TLS
 * context and its executor alive until run() returns. Call stop() or request
 * the run's stop_token and await run() before destroying a derived client.
 * The client's mutable session state and on_text() run on a private strand;
 * send() and stop() may be called from other executor threads.
 *
 * Messages waiting in the bounded channel survive reconnects. A message
 * removed by the writer is never automatically replayed: if its write fails,
 * the server may already have received it. send() acknowledges queue admission,
 * not delivery. Stopping closes the queue and discards undelivered messages.
 */
class StableWebSocketClient {
public:
    StableWebSocketClient(
        boost::asio::any_io_executor executor,
        endpoint::ResolvedEndpoint endpoint,
        StableWebSocketOptions options = {},
        endpoint::ssl_context& tls_context = endpoint::get_global_ssl_context())
        : _strand(boost::asio::make_strand(std::move(executor))),
          _endpoint(std::move(endpoint)),
          _options(options),
          _tls_context(tls_context),
          _control(std::make_shared<Control>(_strand, options.write_capacity))
    {
        if (_endpoint.host.empty() || _endpoint.port.empty() ||
            _options.write_capacity == 0 ||
            _options.initial_backoff.count() <= 0 ||
            _options.max_backoff < _options.initial_backoff ||
            _options.idle_timeout.count() < 0) {
            throw std::invalid_argument("invalid stable WebSocket client configuration");
        }
    }

    virtual ~StableWebSocketClient() = default;
    StableWebSocketClient(const StableWebSocketClient&) = delete;
    StableWebSocketClient& operator=(const StableWebSocketClient&) = delete;

    /**
     * Admit one complete text message to the bounded queue. Suspends when
     * full; throws if the client has stopped or the caller cancels the send.
     */
    boost::asio::awaitable<void> send(std::string message) {
        return enqueue(_control, std::move(message));
    }

    /**
     * Keep connecting until stopped. On a session fault, join both I/O tasks
     * before beginning another connection. An exception from on_text() ends
     * this operation after cleanup and is propagated to the caller.
     */
    boost::asio::awaitable<void> run(std::stop_token stop = {}) {
        co_await boost::asio::co_spawn(
            _strand, run_on_strand(stop), boost::asio::use_awaitable);
    }

    /** Request a prompt stop from any thread; run() is the completion fence. */
    void stop() {
        auto control = _control;
        boost::asio::post(_strand, [control] { request_stop(*control); });
    }

protected:
    /**
     * Called once per complete WebSocket text message on the client's strand.
     * A throwing handler is an application error and ends run(); it is not
     * retried by opening another connection. Do not block this callback.
     */
    virtual void on_text(std::string message) = 0;

private:
    using WriteChannel = boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code, std::string)>;

    struct Session {
        explicit Session(websocket_stream connection)
            : stream(std::move(connection)) {}

        websocket_stream stream;
        boost::asio::cancellation_signal read_cancel;
        boost::asio::cancellation_signal write_cancel;
    };

    struct Control {
        Control(boost::asio::any_io_executor executor, std::size_t capacity)
            : outgoing(std::move(executor), capacity) {}

        WriteChannel outgoing;
        boost::asio::cancellation_signal connect_cancel;
        std::shared_ptr<Session> session;
        std::shared_ptr<boost::asio::steady_timer> backoff_timer;
        bool started = false;
        bool stopping = false;
    };

    enum class TaskKind { TransportFault, HandlerFault };

    struct TaskResult {
        TaskKind kind = TaskKind::TransportFault;
        std::string detail;
        std::exception_ptr exception;
    };

    using ResultChannel = boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code, TaskResult)>;

    static boost::asio::awaitable<void> enqueue(
        std::shared_ptr<Control> control, std::string message)
    {
        co_await control->outgoing.async_send(
            boost::system::error_code{}, std::move(message),
            boost::asio::use_awaitable);
    }

    static void request_stop(Control& control) noexcept {
        if (control.stopping) return;
        control.stopping = true;
        control.outgoing.close();
        control.connect_cancel.emit(boost::asio::cancellation_type::terminal);
        if (control.backoff_timer) control.backoff_timer->cancel();
        if (control.session) {
            control.session->stream.abort();
            control.session->read_cancel.emit(
                boost::asio::cancellation_type::terminal);
            control.session->write_cancel.emit(
                boost::asio::cancellation_type::terminal);
        }
    }

    boost::asio::awaitable<TaskResult> read_messages(
        std::shared_ptr<Session> session)
    {
        try {
            for (;;) {
                boost::beast::flat_buffer buffer;
                co_await session->stream.read(buffer);
                if (!session->stream.got_text()) {
                    co_return TaskResult{TaskKind::TransportFault,
                        "received a binary WebSocket message", {}};
                }
                try {
                    on_text(boost::beast::buffers_to_string(buffer.data()));
                } catch (...) {
                    co_return TaskResult{TaskKind::HandlerFault, {},
                        std::current_exception()};
                }
            }
        } catch (const boost::system::system_error& error) {
            co_return TaskResult{TaskKind::TransportFault, error.what(), {}};
        } catch (const WsException& error) {
            co_return TaskResult{TaskKind::TransportFault, error.what(), {}};
        } catch (...) {
            co_return TaskResult{TaskKind::HandlerFault, {},
                std::current_exception()};
        }
    }

    boost::asio::awaitable<TaskResult> write_messages(
        std::shared_ptr<Session> session)
    {
        try {
            for (;;) {
                std::string message = co_await _control->outgoing.async_receive(
                    boost::asio::use_awaitable);
                try {
                    co_await session->stream.write(std::move(message));
                } catch (const std::exception& error) {
                    logging::Logger::warning(
                        "intercom client: in-flight message has uncertain "
                        "delivery: {}", error.what());
                    throw;
                }
            }
        } catch (const std::exception& error) {
            co_return TaskResult{TaskKind::TransportFault, error.what(), {}};
        }
    }

    boost::asio::awaitable<void> run_session(websocket_stream stream) {
        namespace asio = boost::asio;
        namespace websocket = boost::beast::websocket;

        auto timeout = websocket::stream_base::timeout::suggested(
            boost::beast::role_type::client);
        timeout.handshake_timeout =
            std::chrono::seconds(endpoint::DEFAULT_TIMEOUT_SEC);
        timeout.idle_timeout = _options.idle_timeout.count() == 0
            ? websocket::stream_base::none()
            : _options.idle_timeout;
        timeout.keep_alive_pings = _options.idle_timeout.count() != 0;
        stream.set_option(timeout);

        auto session = std::make_shared<Session>(std::move(stream));
        _control->session = session;
        auto results = std::make_shared<ResultChannel>(_strand, 2);

        auto report = [results](std::exception_ptr error, TaskResult result) {
            if (error) {
                result = TaskResult{TaskKind::HandlerFault, {}, error};
            }
            results->try_send(boost::system::error_code{}, std::move(result));
        };

        asio::co_spawn(_strand, read_messages(session),
            asio::bind_cancellation_slot(session->read_cancel.slot(), report));
        asio::co_spawn(_strand, write_messages(session),
            asio::bind_cancellation_slot(session->write_cancel.slot(), report));

        TaskResult first = co_await results->async_receive(asio::use_awaitable);
        session->stream.abort();
        session->read_cancel.emit(asio::cancellation_type::terminal);
        session->write_cancel.emit(asio::cancellation_type::terminal);
        TaskResult second = co_await results->async_receive(asio::use_awaitable);
        _control->session.reset();

        if (!_control->stopping) {
            if (first.kind == TaskKind::HandlerFault) {
                std::rethrow_exception(first.exception);
            }
            if (second.kind == TaskKind::HandlerFault) {
                std::rethrow_exception(second.exception);
            }
            logging::Logger::warning("intercom client: session ended: {}",
                first.detail);
        }
    }

    boost::asio::awaitable<void> run_on_strand(std::stop_token stop) {
        namespace asio = boost::asio;

        if (_control->started) {
            throw std::logic_error("stable WebSocket client can run only once");
        }
        _control->started = true;
        if (_control->stopping) co_return;
        auto control = _control;
        std::stop_callback on_stop(stop, [control, executor = _strand] {
            asio::post(executor, [control] { request_stop(*control); });
        });

        auto delay = _options.initial_backoff;
        try {
            while (!_control->stopping) {
                try {
                    websocket_stream stream = co_await asio::co_spawn(
                        _strand,
                        connect_websocket(_strand, _endpoint, _tls_context),
                        asio::bind_cancellation_slot(
                            _control->connect_cancel.slot(), asio::use_awaitable));
                    if (_control->stopping) {
                        stream.abort();
                        break;
                    }

                    auto connected_at = std::chrono::steady_clock::now();
                    co_await run_session(std::move(stream));
                    if (std::chrono::steady_clock::now() - connected_at >=
                        std::chrono::seconds(30)) {
                        delay = _options.initial_backoff;
                    }
                } catch (const WsException& error) {
                    if (!_control->stopping) {
                        logging::Logger::warning(
                            "intercom client: connection failed: {}",
                            error.what());
                    }
                } catch (const boost::system::system_error& error) {
                    // Cancellation can be raised by co_spawn itself before
                    // connect_websocket has started and folded the error.
                    if (!_control->stopping) {
                        logging::Logger::warning(
                            "intercom client: connection failed: {}",
                            error.what());
                    }
                }

                if (_control->stopping) break;
                auto timer = std::make_shared<asio::steady_timer>(_strand, delay);
                _control->backoff_timer = timer;
                boost::system::error_code ignored;
                co_await timer->async_wait(
                    asio::redirect_error(asio::use_awaitable, ignored));
                _control->backoff_timer.reset();
                delay = delay >= _options.max_backoff / 2
                    ? _options.max_backoff
                    : delay * 2;
            }
        } catch (...) {
            request_stop(*_control);
            throw;
        }
    }

    boost::asio::strand<boost::asio::any_io_executor> _strand;
    endpoint::ResolvedEndpoint _endpoint;
    StableWebSocketOptions _options;
    endpoint::ssl_context& _tls_context;
    std::shared_ptr<Control> _control;
};

} // namespace intercom
