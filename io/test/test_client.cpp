#define BOOST_TEST_MODULE io_client
#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>

#include "eventbus/event_bus.hpp"
#include "io/client.hpp"
#include "loopback_ws_server.hpp"

namespace {

using namespace std::chrono_literals;

endpoint::ResolvedEndpoint where(unsigned short port) {
    return {.host = "127.0.0.1", .port = std::to_string(port),
            .target = "/io", .tls = false};
}

void write_json(websocket::stream<tcp::socket>& ws, nlohmann::json value) {
    const std::string text = value.dump();
    ws.text(true);
    ws.write(asio::buffer(text));
}

std::exception_ptr drive(asio::io_context& context, io::Client& client,
                         std::chrono::seconds limit = 3s) {
    std::exception_ptr failure;
    bool expired = false;
    asio::steady_timer watchdog(context, limit);
    watchdog.async_wait([&](boost::system::error_code ec) {
        if (!ec) {
            expired = true;
            client.stop();
        }
    });
    asio::co_spawn(context, client.run(), [&](std::exception_ptr error) {
        failure = error;
        watchdog.cancel();
    });
    context.run();
    BOOST_REQUIRE_MESSAGE(!expired, "IO client did not finish before deadline");
    return failure;
}

asio::awaitable<void> take_one(io::Client::PayloadSubscription& subscription,
                               nlohmann::json& received,
                               std::atomic<int>& completed,
                               io::Client& client) {
    received = co_await subscription.next();
    if (completed.fetch_add(1) + 1 == 2) client.stop();
}

asio::awaitable<void> take_and_stop(
    io::Client::PayloadSubscription& subscription,
    nlohmann::json& received,
    std::promise<void>& ready,
    io::Client& client)
{
    received = co_await subscription.next();
    ready.set_value();
    client.stop();
}

asio::awaitable<void> take_two_and_stop(
    io::Client::PayloadSubscription& subscription,
    std::vector<nlohmann::json>& received,
    io::Client& client)
{
    received.push_back(co_await subscription.next());
    received.push_back(co_await subscription.next());
    client.stop();
}

} // namespace

BOOST_AUTO_TEST_CASE(json_send_and_both_routes) {
    nlohmann::json outbound;
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        beast::flat_buffer buffer;
        ws.read(buffer);
        outbound = nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
        write_json(ws, {{"type", "payload"}, {"data", {{"text", "hi"}}}});
        write_json(ws, {{"type", "signal"}, {"data", {{"name", "cancel"}}}});
    });

    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()), bus);
    auto subscription = client.subscribe_payload();
    nlohmann::json payload;
    nlohmann::json signal;
    std::atomic<int> completed{0};
    eventbus::EventBus::ScopedSubscription signal_subscription{
        bus.subscribe<io::SignalEvent>([&](const io::SignalEvent& event) {
            signal = event.signal;
            if (completed.fetch_add(1) + 1 == 2) client.stop();
        })};
    asio::co_spawn(context, take_one(subscription, payload, completed, client),
                   asio::detached);
    asio::co_spawn(context, client.send({{"type", "reply"}, {"data", 42}}),
                   asio::detached);

    auto failure = drive(context, client);
    server.join();
    BOOST_CHECK(!failure);
    BOOST_TEST(outbound.dump() ==
               nlohmann::json({{"type", "reply"}, {"data", 42}}).dump());
    BOOST_TEST(payload.dump() == nlohmann::json({{"text", "hi"}}).dump());
    BOOST_TEST(signal.dump() == nlohmann::json({{"name", "cancel"}}).dump());
}

BOOST_AUTO_TEST_CASE(history_payload_bypasses_run_input_queue) {
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        write_json(ws, {{"type", "payload"}, {"data", {
            {"operation", "history"}, {"request_id", "history-1"}}}});
        write_json(ws, {{"type", "payload"}, {"data", {
            {"operation", "message"}, {"request_id", "message-1"}}}});
    });

    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()), bus);
    auto subscription = client.subscribe_payload();
    nlohmann::json payload;
    nlohmann::json query;
    std::atomic<int> completed{0};
    eventbus::EventBus::ScopedSubscription query_subscription{
        bus.subscribe<io::PayloadQueryEvent>([&](const io::PayloadQueryEvent& event) {
            query = event.payload;
            if (completed.fetch_add(1) + 1 == 2) client.stop();
        })};
    asio::co_spawn(context, take_one(subscription, payload, completed, client),
                   asio::detached);
    auto failure = drive(context, client);
    server.join();
    BOOST_CHECK(!failure);
    BOOST_TEST(query.at("request_id") == "history-1");
    BOOST_TEST(payload.at("request_id") == "message-1");
}

BOOST_AUTO_TEST_CASE(slow_signal_handler_does_not_block_payload_route) {
    std::promise<void> handler_started;
    auto started = handler_started.get_future();
    bool server_saw_handler = false;
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        write_json(ws, {{"type", "signal"}, {"data", "wait"}});
        server_saw_handler = started.wait_for(2s) == std::future_status::ready;
        write_json(ws, {{"type", "payload"}, {"data", "request"}});
    });

    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()), bus);
    auto subscription = client.subscribe_payload();
    std::promise<void> payload_arrived;
    auto ready = payload_arrived.get_future();
    std::atomic<bool> handler_unblocked{false};
    client.register_signal_handler([&](const nlohmann::json&) {
        handler_started.set_value();
        handler_unblocked = ready.wait_for(2s) == std::future_status::ready;
    });
    nlohmann::json payload;
    asio::co_spawn(context,
        take_and_stop(subscription, payload, payload_arrived, client),
        asio::detached);

    auto failure = drive(context, client);
    server.join();
    BOOST_CHECK(!failure);
    BOOST_CHECK(server_saw_handler);
    BOOST_CHECK(handler_unblocked.load());
    BOOST_TEST(payload.dump() == nlohmann::json("request").dump());
}

BOOST_AUTO_TEST_CASE(full_payload_queue_keeps_signal_route_live) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        write_json(ws, {{"type", "payload"}, {"data", 1}});
        write_json(ws, {{"type", "payload"}, {"data", 2}});
        write_json(ws, {{"type", "signal"}, {"data", "stop"}});
    });

    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()),
                      bus, {.payload_capacity = 1, .signal_capacity = 8});
    auto subscription = client.subscribe_payload();
    bool signal_seen = false;
    eventbus::EventBus::ScopedSubscription signal_subscription{
        bus.subscribe<io::SignalEvent>([&](const io::SignalEvent&) {
            signal_seen = true;
            client.stop();
        })};

    auto failure = drive(context, client);
    server.join();
    BOOST_CHECK(!failure);
    BOOST_CHECK(signal_seen);
    BOOST_TEST(client.rejected_payloads() == 1u);
}

BOOST_AUTO_TEST_CASE(overflow_reports_metadata_and_preserves_payload_fifo) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        for (const auto& data : std::vector<nlohmann::json>{
                1, 2,
                {{"request_id", "rejected"}, {"operation", "compact"},
                 {"content", std::string(1024, 'x')}},
                nullptr,
                {{"request_id", 42}, {"operation", nlohmann::json::object()}}}) {
            write_json(ws, {{"type", "payload"}, {"data", data}});
        }
        write_json(ws, {{"type", "signal"}, {"data", "drain"}});
    });
    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()),
                      bus, {.payload_capacity = 2, .signal_capacity = 8});
    auto subscription = client.subscribe_payload();
    std::vector<io::PayloadRejectedEvent> rejected;
    std::vector<nlohmann::json> received;
    eventbus::EventBus::ScopedSubscription rejection_subscription{
        bus.subscribe<io::PayloadRejectedEvent>([&](const auto& event) {
            rejected.push_back(event);
        })};
    eventbus::EventBus::ScopedSubscription signal_subscription{
        bus.subscribe<io::SignalEvent>([&](const auto&) {
            asio::co_spawn(context, take_two_and_stop(subscription, received, client),
                           asio::detached);
        })};
    const auto failure = drive(context, client);
    server.join();
    BOOST_CHECK(!failure);
    BOOST_TEST(client.rejected_payloads() == 3u);
    BOOST_REQUIRE_EQUAL(rejected.size(), 3u);
    BOOST_TEST(rejected[0].request_id == "rejected");
    BOOST_TEST(rejected[0].operation == "compact");
    BOOST_TEST(rejected[1].request_id.is_null());
    BOOST_TEST(rejected[1].operation.is_null());
    BOOST_TEST(rejected[2].request_id == 42);
    BOOST_TEST(rejected[2].operation.is_null());
    BOOST_REQUIRE_EQUAL(received.size(), 2u);
    BOOST_TEST(received[0] == 1);
    BOOST_TEST(received[1] == 2);
}

BOOST_AUTO_TEST_CASE(full_rejection_control_queue_is_fatal_and_counted) {
    std::promise<void> handler_started;
    auto started = handler_started.get_future();
    std::promise<void> release_handler;
    auto release = release_handler.get_future();
    bool server_saw_handler = false;
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        write_json(ws, {{"type", "signal"}, {"data", "hold"}});
        server_saw_handler = started.wait_for(2s) == std::future_status::ready;
        for (int index = 0; index != 3; ++index) {
            write_json(ws, {{"type", "payload"}, {"data", index}});
        }
        // The third payload cannot queue its rejection. Transport shutdown
        // wakes this read; only then release the deliberately occupied worker.
        beast::flat_buffer buffer;
        boost::system::error_code error;
        ws.read(buffer, error);
        release_handler.set_value();
    });
    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()),
                      bus, {.payload_capacity = 1, .signal_capacity = 1});
    client.register_signal_handler([&](const auto&) {
        handler_started.set_value();
        release.wait_for(2s);
    });
    const auto failure = drive(context, client);
    server.join();
    BOOST_CHECK(server_saw_handler);
    BOOST_TEST(client.rejected_payloads() == 2u);
    BOOST_REQUIRE(failure);
    BOOST_CHECK_EXCEPTION(std::rethrow_exception(failure), std::runtime_error,
        [](const auto& error) {
            return std::string(error.what()).find("reporting payload rejection")
                != std::string::npos;
        });
}

BOOST_AUTO_TEST_CASE(throwing_rejection_listener_ends_run) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        write_json(ws, {{"type", "payload"}, {"data", 1}});
        write_json(ws, {{"type", "payload"}, {"data", 2}});
    });
    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()),
                      bus, {.payload_capacity = 1, .signal_capacity = 8});
    eventbus::EventBus::ScopedSubscription subscription{
        bus.subscribe<io::PayloadRejectedEvent>([](const auto&) {
            throw std::runtime_error("rejection listener failed");
        })};
    const auto failure = drive(context, client);
    server.join();
    BOOST_REQUIRE(failure);
    BOOST_CHECK_EXCEPTION(std::rethrow_exception(failure), std::runtime_error,
        [](const auto& error) {
            return std::string(error.what()) == "rejection listener failed";
        });
}

BOOST_AUTO_TEST_CASE(payload_subscription_survives_reconnect) {
    loopback_ws::SequenceServer server({
        [](tcp::socket& socket) {
            websocket::stream<tcp::socket> ws(std::move(socket));
            ws.accept();
            write_json(ws, {{"type", "payload"}, {"data", 1}});
        },
        [](tcp::socket& socket) {
            websocket::stream<tcp::socket> ws(std::move(socket));
            ws.accept();
            write_json(ws, {{"type", "payload"}, {"data", 2}});
        }
    });

    asio::io_context context;
    eventbus::EventBus bus;
    intercom::StableWebSocketOptions transport;
    transport.initial_backoff = 20ms;
    transport.max_backoff = 40ms;
    io::Client client(context.get_executor(), where(server.wait_listening()),
                      bus, {}, transport);
    auto subscription = client.subscribe_payload();
    std::vector<nlohmann::json> received;
    asio::co_spawn(context,
        take_two_and_stop(subscription, received, client), asio::detached);

    auto failure = drive(context, client);
    server.join();
    BOOST_CHECK(!failure);
    BOOST_TEST(received.size() == 2u);
    if (received.size() == 2) {
        BOOST_TEST(received[0] == 1);
        BOOST_TEST(received[1] == 2);
    }
}

BOOST_AUTO_TEST_CASE(malformed_envelope_ends_run) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        ws.text(true);
        ws.write(asio::buffer("not json", 8));
    });

    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()), bus);
    auto failure = drive(context, client);
    server.join();
    BOOST_REQUIRE(failure);
    BOOST_CHECK_THROW(std::rethrow_exception(failure), nlohmann::json::parse_error);
}

BOOST_AUTO_TEST_CASE(payload_subscription_rejects_overlapping_next_calls) {
    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(1), bus);
    auto subscription = client.subscribe_payload();
    std::exception_ptr first_failure;
    std::exception_ptr second_failure;

    asio::co_spawn(context, subscription.next(),
        [&](std::exception_ptr error, nlohmann::json) {
            first_failure = error;
        });
    asio::steady_timer gate(context, 1ms);
    gate.async_wait([&](boost::system::error_code ec) {
        if (ec) return;
        asio::co_spawn(context, subscription.next(),
            [&](std::exception_ptr error, nlohmann::json) {
                second_failure = error;
                client.stop();
            });
    });

    context.run();
    BOOST_REQUIRE(first_failure);
    BOOST_REQUIRE(second_failure);
    BOOST_CHECK_THROW(std::rethrow_exception(second_failure), std::logic_error);
}

BOOST_AUTO_TEST_CASE(throwing_signal_handler_ends_run) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        write_json(ws, {{"type", "signal"}, {"data", "bad"}});
    });

    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()), bus);
    client.register_signal_handler([](const nlohmann::json&) {
        throw std::runtime_error("signal handler failed");
    });
    auto failure = drive(context, client);
    server.join();
    BOOST_REQUIRE(failure);
    BOOST_CHECK_THROW(std::rethrow_exception(failure), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(full_signal_queue_ends_run_without_silent_loss) {
    std::promise<void> handler_started;
    auto started = handler_started.get_future();
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        write_json(ws, {{"type", "signal"}, {"data", 1}});
        started.wait_for(2s);
        write_json(ws, {{"type", "signal"}, {"data", 2}});
        write_json(ws, {{"type", "signal"}, {"data", 3}});
    });

    asio::io_context context;
    eventbus::EventBus bus;
    io::Client client(context.get_executor(), where(server.wait_listening()),
                      bus, {.payload_capacity = 1, .signal_capacity = 1});
    std::promise<void> release_handler;
    auto release = release_handler.get_future();
    std::atomic<int> handled{0};
    client.register_signal_handler([&](const nlohmann::json&) {
        if (handled.fetch_add(1) == 0) handler_started.set_value();
        release.wait();
    });
    asio::steady_timer release_timer(context, 80ms);
    release_timer.async_wait([&](boost::system::error_code) {
        release_handler.set_value();
    });

    auto failure = drive(context, client);
    server.join();
    BOOST_REQUIRE(failure);
    try {
        std::rethrow_exception(failure);
    } catch (const std::runtime_error& error) {
        BOOST_TEST(std::string(error.what()).find("signal queue is full") !=
                   std::string::npos);
    }
}
