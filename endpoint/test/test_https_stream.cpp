#define BOOST_TEST_MODULE HttpsStreamTests
#include <boost/test/unit_test.hpp>

#include "endpoint/https_stream.hpp"

#include <array>
#include <string>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

BOOST_AUTO_TEST_CASE(global_context_is_a_thread_safe_singleton)
{
    constexpr std::size_t thread_count = 8;
    std::array<endpoint::ssl_context*, thread_count> contexts{};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([index, &contexts] {
            contexts[index] = &endpoint::get_global_ssl_context();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    for (auto* context : contexts) {
        BOOST_TEST(context == contexts.front());
    }
}

BOOST_AUTO_TEST_CASE(connection_refusal_is_reported)
{
    asio::io_context reservation_io;
    tcp::acceptor reservation(
        reservation_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto unused_port = reservation.local_endpoint().port();
    reservation.close();

    asio::io_context io;
    auto operation = endpoint::create_https_connection_stream(
        io.get_executor(), "127.0.0.1", std::to_string(unused_port));
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();

    BOOST_CHECK_THROW(result.get(), boost::system::system_error);
}

BOOST_AUTO_TEST_CASE(non_tls_peer_causes_handshake_failure)
{
    asio::io_context server_io;
    tcp::acceptor acceptor(
        server_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();

    std::thread server([&acceptor] {
        tcp::socket socket(acceptor.get_executor());
        acceptor.accept(socket);
        const std::string response = "this is not a TLS record";
        boost::system::error_code ignored;
        asio::write(socket, asio::buffer(response), ignored);
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    });

    asio::io_context client_io;
    auto operation = endpoint::create_https_connection_stream(
        client_io.get_executor(), "localhost", std::to_string(port));
    auto result =
        asio::co_spawn(client_io, std::move(operation), asio::use_future);
    client_io.run();

    BOOST_CHECK_THROW(result.get(), boost::system::system_error);
    server.join();
}

BOOST_AUTO_TEST_CASE(unknown_service_is_reported)
{
    asio::io_context io;
    auto operation = []() -> asio::awaitable<void> {
        co_await endpoint::create_https_connection_stream(
            "localhost", "not-a-real-service-name");
    }();
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();

    BOOST_CHECK_THROW(result.get(), boost::system::system_error);
}
