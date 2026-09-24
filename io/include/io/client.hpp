#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <nlohmann/json.hpp>

#include "eventbus/event_bus.hpp"
#include "intercom/stable_websocket_client.hpp"

namespace io {

/** The generic synchronous event emitted for a server signal. */
struct SignalEvent {
    nlohmann::json signal;
};

/** Capacities of the two incoming queues. Both must be positive. */
struct ClientOptions {
    std::size_t payload_capacity = 64;
    std::size_t signal_capacity = 64;
};

/**
 * @brief Route JSON messages from one stable text WebSocket connection.
 *
 * Wire messages are objects with the required routing fields `type` and
 * `data`: type is "payload" or "signal"; data is any JSON value. Additional
 * fields are allowed. Payloads wait for one explicit consumer. Signals go to
 * a dedicated worker thread, so synchronous bus listeners cannot occupy the
 * WebSocket reader's thread. The injected EventBus must outlive run().
 *
 * The intercom base remains a plain-text transport. This class alone parses
 * incoming JSON and serializes outgoing JSON. A malformed envelope ends
 * run() as a protocol error. Once run() returns, both incoming queues are
 * closed and the signal worker has been joined. Keep this object, its payload
 * subscription, its EventBus and its executor alive until run() returns.
 */
class Client final : public intercom::StableWebSocketClient {
private:
    using JsonChannel = boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code, nlohmann::json)>;
    using DoneChannel = boost::asio::experimental::concurrent_channel<
        void(boost::system::error_code, std::exception_ptr)>;

    struct State {
        State(boost::asio::any_io_executor payload_executor,
              boost::asio::any_io_executor signal_executor,
              ClientOptions options)
            : payloads(std::move(payload_executor), options.payload_capacity),
              signals(std::move(signal_executor), options.signal_capacity) {}

        JsonChannel payloads;
        JsonChannel signals;
        std::atomic<bool> subscribed{false};
        std::atomic<bool> receiving{false};
        std::atomic<bool> stopping{false};
        std::atomic<std::size_t> rejected_payloads{0};
    };

public:
    /** One exclusive consumer of the payload queue. Move-only. */
    class PayloadSubscription {
    public:
        PayloadSubscription(PayloadSubscription&&) noexcept = default;
        PayloadSubscription& operator=(PayloadSubscription&&) noexcept = default;
        PayloadSubscription(const PayloadSubscription&) = delete;
        PayloadSubscription& operator=(const PayloadSubscription&) = delete;

        /**
         * Wait for and remove the next user request; a closed queue throws.
         * Only one next() call may be outstanding on this subscription.
         */
        boost::asio::awaitable<nlohmann::json> next();

    private:
        friend class Client;
        explicit PayloadSubscription(std::shared_ptr<State> state)
            : _state(std::move(state)) {}
        std::shared_ptr<State> _state;
    };

    using SignalHandler = std::function<void(const nlohmann::json&)>;

    Client(boost::asio::any_io_executor executor,
           endpoint::ResolvedEndpoint endpoint,
           eventbus::EventBus& events,
           ClientOptions options = {},
           intercom::StableWebSocketOptions transport_options = {},
           endpoint::ssl_context& tls_context =
               endpoint::get_global_ssl_context());

    ~Client() override;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /** Serialize one JSON value and admit its text to the transport queue. */
    boost::asio::awaitable<void> send(nlohmann::json message);

    /** Start the signal worker and the transport; one invocation per client. */
    boost::asio::awaitable<void> run(std::stop_token stop = {});

    /** Stop transport and both incoming queues; run() is the completion fence. */
    void stop();

    /** Claim the payload queue's single consumer slot. May be called once. */
    PayloadSubscription subscribe_payload();

    /** Replace the default bus publisher with a synchronous JSON handler. */
    void register_signal_handler(SignalHandler handler);

    /** Count payloads explicitly rejected because their queue was full. */
    [[nodiscard]] std::size_t rejected_payloads() const noexcept;

protected:
    void on_text(std::string message) override;

private:
    boost::asio::awaitable<void> process_signals();
    void close_queues() noexcept;

    boost::asio::io_context _signal_io;
    std::shared_ptr<State> _state;
    DoneChannel _signal_done;
    std::jthread _signal_thread;
    std::mutex _handler_mutex;
    SignalHandler _signal_handler;
    std::atomic<bool> _started{false};
};

} // namespace io
