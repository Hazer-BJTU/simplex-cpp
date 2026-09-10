// Deterministic, offline unit tests for the plain-HTTP connection factory:
// the same loopback-only shape as test_https_stream, minus the TLS handshake.
#define BOOST_TEST_MODULE HttpStreamTests
#include <boost/test/unit_test.hpp>

#include "endpoint/https_stream.hpp"

#include <algorithm>
#include <string>
#include <thread>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

// The unified factory's flavour selection is a compile-time property.
static_assert(!endpoint::is_tls_stream_v<endpoint::http_stream>);
static_assert(
    endpoint::default_connection_port_v<endpoint::http_stream> ==
    endpoint::DEFAULT_HTTP_PORT);

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

BOOST_AUTO_TEST_CASE(unified_factory_connects_the_plain_flavour)
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
    auto operation = endpoint::create_connection_stream<endpoint::http_stream>(
        io.get_executor(), "localhost", std::to_string(port));
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    auto stream = result.get();   // rethrows a connect failure, if any

    BOOST_REQUIRE(stream != nullptr);
    boost::system::error_code endpoint_ec;
    const tcp::endpoint peer = stream->socket().remote_endpoint(endpoint_ec);
    BOOST_CHECK(!endpoint_ec);
    BOOST_CHECK_EQUAL(peer.port(), port);

    stream->close();
    server.join();
}

// The coroutine owns the strings it connects with.
//
// create_connection_stream returns a LAZY awaitable: its body runs when the
// caller awaits it (or hands it to co_spawn), by which point the caller's
// full-expression is over. A `std::string_view port` parameter therefore
// borrowed storage the caller may already have reused — the usual shape being
// `create_connection_stream(ex, "host", std::to_string(port))`, where the
// temporary dies at the semicolon.
//
// This test reuses the caller's storage deliberately, before the body ever
// runs, so the defect is visible WITHOUT a sanitizer: with a borrowed view the
// resolver is handed "XXXXX" and the connect fails; with the by-value parameter
// the frame's own copy is untouched and the connection lands on the port below.
// (ASAN reported the borrowed version as stack-use-after-scope inside
// connect_tcp's async_resolve, which is how it was found.)
BOOST_AUTO_TEST_CASE(a_transient_service_name_reaches_the_port_below)
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

    // The caller's service-name storage, exactly what std::to_string(port)
    // leaves behind once the temporary is gone.
    std::string service = std::to_string(port);

    asio::io_context io;
    auto operation = endpoint::create_connection_stream<endpoint::http_stream>(
        io.get_executor(), "localhost", service);
    // The awaitable exists; its body has NOT run. Reuse the caller's buffer —
    // which a borrowed view would be reading.
    std::fill_n(service.data(), service.size(), 'X');

    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    auto stream = result.get();   // rethrows a connect failure, if any

    BOOST_REQUIRE(stream != nullptr);
    boost::system::error_code endpoint_ec;
    const tcp::endpoint peer = stream->socket().remote_endpoint(endpoint_ec);
    BOOST_CHECK(!endpoint_ec);
    BOOST_CHECK_EQUAL(peer.port(), port);

    stream->close();
    server.join();
}

BOOST_AUTO_TEST_CASE(unified_factory_reports_unknown_service)
{
    asio::io_context io;
    auto operation = []() -> asio::awaitable<void> {
        // this_coro::executor convenience form of the unified factory; the
        // context parameter keeps the single call shape and is ignored by the
        // plain flavour.
        co_await endpoint::create_connection_stream<endpoint::http_stream>(
            "localhost", "not-a-real-service-name");
    }();
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();

    BOOST_CHECK_THROW(result.get(), boost::system::system_error);
}
