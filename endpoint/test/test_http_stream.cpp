// Deterministic, offline unit tests for the plain-HTTP connection factory:
// the same loopback-only shape as test_https_stream, minus the TLS handshake.
#define BOOST_TEST_MODULE HttpStreamTests
#include <boost/test/unit_test.hpp>

#include "endpoint/https_stream.hpp"

#include <string>
#include <thread>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

BOOST_AUTO_TEST_CASE(connection_refusal_is_reported)
{
    asio::io_context reservation_io;
    tcp::acceptor reservation(
        reservation_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto unused_port = reservation.local_endpoint().port();
    reservation.close();

    asio::io_context io;
    auto operation = endpoint::create_http_connection_stream(
        io.get_executor(), "127.0.0.1", std::to_string(unused_port));
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();

    BOOST_CHECK_THROW(result.get(), boost::system::system_error);
}

BOOST_AUTO_TEST_CASE(connects_to_loopback_listener)
{
    asio::io_context server_io;
    tcp::acceptor acceptor(
        server_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();

    std::thread server([&acceptor] {
        tcp::socket socket(acceptor.get_executor());
        acceptor.accept(socket);
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    });

    asio::io_context io;
    auto operation = endpoint::create_http_connection_stream(
        io.get_executor(), "localhost", std::to_string(port));
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    auto stream = result.get();   // rethrows a connect failure, if any

    BOOST_REQUIRE(stream != nullptr);
    // Liveness: the connected stream's socket has a peer endpoint.
    boost::system::error_code endpoint_ec;
    const tcp::endpoint peer = stream->socket().remote_endpoint(endpoint_ec);
    BOOST_CHECK(!endpoint_ec);
    BOOST_CHECK_EQUAL(peer.address().to_string(), "127.0.0.1");
    BOOST_CHECK_EQUAL(peer.port(), port);

    stream->close();
    server.join();
}

BOOST_AUTO_TEST_CASE(unknown_service_is_reported)
{
    asio::io_context io;
    auto operation = []() -> asio::awaitable<void> {
        // this_coro::executor convenience overload.
        co_await endpoint::create_http_connection_stream(
            "localhost", "not-a-real-service-name");
    }();
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();

    BOOST_CHECK_THROW(result.get(), boost::system::system_error);
}
