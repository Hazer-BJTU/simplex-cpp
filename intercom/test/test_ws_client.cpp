#define BOOST_TEST_MODULE ws_client
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/beast/websocket.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "intercom/stable_websocket_client.hpp"
#include "loopback_ws_server.hpp"

namespace {

using namespace std::chrono_literals;

endpoint::ResolvedEndpoint where(unsigned short port) {
    return {.host = "127.0.0.1", .port = std::to_string(port),
            .target = "/client", .tls = false};
}

intercom::StableWebSocketOptions fast_options() {
    return {.write_capacity = 4, .initial_backoff = 80ms,
            .max_backoff = 160ms, .idle_timeout = 0s};
}

class Client final : public intercom::StableWebSocketClient {
public:
    using StableWebSocketClient::StableWebSocketClient;

    std::vector<std::string> received;
    std::function<void(const std::string&)> on_receive;

protected:
    void on_text(std::string message) override {
        received.push_back(std::move(message));
        if (on_receive) on_receive(received.back());
    }
};

asio::awaitable<void> send_two(Client& client) {
    co_await client.send("first");
    co_await client.send("second");
}

asio::awaitable<void> send_after_delay(Client& client) {
    asio::steady_timer timer(co_await asio::this_coro::executor, 20ms);
    co_await timer.async_wait(asio::use_awaitable);
    co_await client.send("queued");
}

asio::awaitable<void> send_large_then_next(Client& client) {
    co_await client.send(std::string(8 * 1024 * 1024, 'x'));
    co_await client.send("after-reset");
}

asio::awaitable<void> send_after_silence(Client& client) {
    asio::steady_timer timer(co_await asio::this_coro::executor, 2200ms);
    co_await timer.async_wait(asio::use_awaitable);
    co_await client.send("still-here");
}

// The deadline turns a failed lifecycle test into an assertion rather than
// leaving ctest blocked on a live client coroutine.
std::exception_ptr drive(asio::io_context& io, Client& client,
                         std::chrono::seconds limit = 3s) {
    std::exception_ptr error;
    bool timed_out = false;
    asio::steady_timer deadline(io, limit);
    deadline.async_wait([&](boost::system::error_code ec) {
        if (!ec) {
            timed_out = true;
            client.stop();
        }
    });
    asio::co_spawn(io, client.run(), [&](std::exception_ptr failure) {
        error = failure;
        deadline.cancel();
    });
    io.run();
    BOOST_REQUIRE_MESSAGE(!timed_out, "client did not stop before deadline");
    return error;
}

} // namespace

BOOST_AUTO_TEST_CASE(two_way_text_messages_keep_one_connection) {
    std::vector<std::string> written;
    bool all_text = true;
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        for (int i = 0; i < 2; ++i) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            all_text &= ws.got_text();
            written.push_back(beast::buffers_to_string(buffer.data()));
            ws.text(true);
            std::string reply = "reply-" + std::to_string(i);
            ws.write(asio::buffer(reply));
        }
    });

    asio::io_context io;
    Client client(io.get_executor(), where(server.wait_listening()), fast_options());
    client.on_receive = [&](const std::string&) {
        if (client.received.size() == 2) client.stop();
    };
    asio::co_spawn(io, send_two(client), asio::detached);

    auto error = drive(io, client);
    server.join();
    BOOST_CHECK(!error);
    BOOST_CHECK(all_text);
    BOOST_TEST(written == std::vector<std::string>({"first", "second"}));
    BOOST_TEST(client.received ==
               std::vector<std::string>({"reply-0", "reply-1"}));
}

BOOST_AUTO_TEST_CASE(read_failure_reconnects_and_delivers_queued_message) {
    std::string second_message;
    loopback_ws::SequenceServer server({
        [](tcp::socket& socket) {
            websocket::stream<tcp::socket> ws(std::move(socket));
            ws.accept();
            ws.text(true);
            ws.write(asio::buffer("first", 5));
        },
        [&](tcp::socket& socket) {
            websocket::stream<tcp::socket> ws(std::move(socket));
            ws.accept();
            beast::flat_buffer buffer;
            ws.read(buffer);
            second_message = beast::buffers_to_string(buffer.data());
            ws.text(true);
            ws.write(asio::buffer("second", 6));
        }
    });

    asio::io_context io;
    Client client(io.get_executor(), where(server.wait_listening()), fast_options());
    client.on_receive = [&](const std::string& text) {
        if (text == "first") {
            // The first peer has closed by the time this timer fires. The
            // message waits in the same queue through the reconnect delay.
            asio::co_spawn(io, send_after_delay(client), asio::detached);
        } else if (text == "second") {
            client.stop();
        }
    };

    auto error = drive(io, client);
    server.join();
    BOOST_CHECK(!error);
    BOOST_TEST(second_message == "queued");
    BOOST_TEST(client.received ==
               std::vector<std::string>({"first", "second"}));
}

BOOST_AUTO_TEST_CASE(upgrade_failure_reconnects_without_consuming_queue) {
    bool text = false;
    loopback_ws::SequenceServer server({
        [](tcp::socket& socket) {
            loopback_ws::serve_reject(socket, http::status::service_unavailable,
                                      "temporary outage");
        },
        [&](tcp::socket& socket) {
            loopback_ws::serve_echo(socket, &text);
        }
    });

    asio::io_context io;
    Client client(io.get_executor(), where(server.wait_listening()), fast_options());
    client.on_receive = [&](const std::string&) { client.stop(); };
    asio::co_spawn(io, client.send("kept"), asio::detached);

    auto error = drive(io, client);
    server.join();
    BOOST_CHECK(!error);
    BOOST_CHECK(text);
    BOOST_TEST(client.received == std::vector<std::string>({"kept"}));
}

BOOST_AUTO_TEST_CASE(reset_during_large_write_does_not_replay_in_flight_message) {
    std::string second_message;
    loopback_ws::SequenceServer server({
        [](tcp::socket& socket) {
            websocket::stream<tcp::socket> ws(std::move(socket));
            ws.accept();
            // Observe the beginning of the large frame, then reset the
            // socket while the client is still writing the rest of it.
            std::array<char, 4096> bytes;
            ws.next_layer().read_some(asio::buffer(bytes));
            ws.next_layer().set_option(asio::socket_base::linger(true, 0));
        },
        [&](tcp::socket& socket) {
            websocket::stream<tcp::socket> ws(std::move(socket));
            ws.accept();
            beast::flat_buffer buffer;
            ws.read(buffer);
            second_message = beast::buffers_to_string(buffer.data());
            ws.text(true);
            ws.write(asio::buffer("ack", 3));
        }
    });

    asio::io_context io;
    Client client(io.get_executor(), where(server.wait_listening()), fast_options());
    client.on_receive = [&](const std::string&) { client.stop(); };
    asio::co_spawn(io, send_large_then_next(client), asio::detached);

    auto error = drive(io, client);
    server.join();
    BOOST_CHECK(!error);
    BOOST_TEST(second_message == "after-reset");
    BOOST_TEST(client.received == std::vector<std::string>({"ack"}));
}

BOOST_AUTO_TEST_CASE(configured_idle_ping_keeps_a_silent_peer_connected) {
    std::string received_by_server;
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        beast::flat_buffer buffer;
        ws.read(buffer); // processes idle pings and automatically sends pongs
        received_by_server = beast::buffers_to_string(buffer.data());
        ws.text(true);
        ws.write(asio::buffer("ack", 3));
    });

    asio::io_context io;
    auto options = fast_options();
    options.idle_timeout = 1s;
    Client client(io.get_executor(), where(server.wait_listening()), options);
    client.on_receive = [&](const std::string&) { client.stop(); };
    asio::co_spawn(io, send_after_silence(client), asio::detached);

    auto error = drive(io, client, 5s);
    server.join();
    BOOST_CHECK(!error);
    BOOST_TEST(received_by_server == "still-here");
    BOOST_TEST(client.received == std::vector<std::string>({"ack"}));
}

BOOST_AUTO_TEST_CASE(default_timeout_keeps_a_silent_session_open_without_ping) {
    bool saw_ping = false;
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        ws.control_callback([&](websocket::frame_type type,
                                beast::string_view) {
            if (type == websocket::frame_type::ping) saw_ping = true;
        });
        beast::flat_buffer buffer;
        ws.read(buffer);
        ws.text(true);
        ws.write(asio::buffer("ack", 3));
    });

    asio::io_context io;
    Client client(io.get_executor(), where(server.wait_listening()));
    client.on_receive = [&](const std::string&) { client.stop(); };
    asio::co_spawn(io, send_after_silence(client), asio::detached);

    auto error = drive(io, client, 5s);
    server.join();
    BOOST_CHECK(!error);
    BOOST_CHECK(!saw_ping);
    BOOST_TEST(client.received == std::vector<std::string>({"ack"}));
}

BOOST_AUTO_TEST_CASE(stop_interrupts_connected_reader_and_writer) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        beast::flat_buffer buffer;
        boost::system::error_code ignored;
        ws.read(buffer, ignored); // client closes the transport on stop
    });

    asio::io_context io;
    Client client(io.get_executor(), where(server.wait_listening()), fast_options());
    asio::steady_timer stop_timer(io, 30ms);
    stop_timer.async_wait([&](boost::system::error_code ec) {
        if (!ec) client.stop();
    });

    auto error = drive(io, client);
    server.join();
    BOOST_CHECK(!error);
    BOOST_CHECK(client.received.empty());
}

BOOST_AUTO_TEST_CASE(stop_interrupts_retry_backoff) {
    asio::io_context probe_io;
    tcp::acceptor probe(probe_io,
        tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = probe.local_endpoint().port();
    probe.close();

    asio::io_context io;
    Client client(io.get_executor(), where(port), fast_options());
    asio::steady_timer stop_timer(io, 30ms);
    stop_timer.async_wait([&](boost::system::error_code ec) {
        if (!ec) client.stop();
    });
    BOOST_CHECK(!drive(io, client));
}

BOOST_AUTO_TEST_CASE(stop_before_run_starts_returns_cleanly) {
    asio::io_context io;
    Client client(io.get_executor(), where(1), fast_options());
    client.stop();
    BOOST_CHECK(!drive(io, client));
}

BOOST_AUTO_TEST_CASE(handler_exception_ends_run_after_joining_tasks) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        ws.text(true);
        ws.write(asio::buffer("poison", 6));
    });

    asio::io_context io;
    Client client(io.get_executor(), where(server.wait_listening()), fast_options());
    client.on_receive = [](const std::string&) {
        throw std::runtime_error("handler failed");
    };
    auto error = drive(io, client);
    server.join();
    BOOST_REQUIRE(error);
    BOOST_CHECK_THROW(std::rethrow_exception(error), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(stop_token_interrupts_websocket_handshake) {
    loopback_ws::OneShotServer server([](tcp::socket&) {
        // Leave the HTTP upgrade unanswered. The client must cancel its
        // pending handshake rather than wait for Beast's full deadline.
        std::this_thread::sleep_for(200ms);
    });

    asio::io_context io;
    Client client(io.get_executor(), where(server.wait_listening()), fast_options());
    std::stop_source source;
    std::exception_ptr error;
    asio::steady_timer stop_timer(io, 30ms);
    stop_timer.async_wait([&](boost::system::error_code ec) {
        if (!ec) source.request_stop();
    });
    asio::co_spawn(io, client.run(source.get_token()),
        [&](std::exception_ptr failure) { error = failure; });
    auto started = std::chrono::steady_clock::now();
    io.run();
    auto elapsed = std::chrono::steady_clock::now() - started;
    server.join();

    BOOST_CHECK(!error);
    BOOST_CHECK(elapsed < 150ms);
}

BOOST_AUTO_TEST_CASE(stop_from_another_thread_joins_active_session) {
    std::promise<void> accepted;
    auto connection_ready = accepted.get_future();
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept();
        accepted.set_value();
        beast::flat_buffer buffer;
        boost::system::error_code ignored;
        ws.read(buffer, ignored);
    });

    asio::io_context io;
    Client client(io.get_executor(), where(server.wait_listening()), fast_options());
    std::promise<std::exception_ptr> completed;
    auto result = completed.get_future();
    asio::co_spawn(io, client.run(),
        [&](std::exception_ptr failure) { completed.set_value(failure); });

    std::thread first([&] { io.run(); });
    std::thread second([&] { io.run(); });
    const auto ready = connection_ready.wait_for(2s);
    client.stop();
    const auto finished = result.wait_for(2s);
    io.stop();
    first.join();
    second.join();
    server.join();

    BOOST_REQUIRE(ready == std::future_status::ready);
    BOOST_REQUIRE(finished == std::future_status::ready);
    BOOST_CHECK(!result.get());
}
