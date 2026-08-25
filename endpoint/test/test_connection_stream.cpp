// Unit tests for endpoint::connection_stream — the runtime-flavour facade —
// and for create_connection_stream(executor, resolved), the factory that
// makes the runtime scheme choice. Loopback-only, offline.
#define BOOST_TEST_MODULE connection_stream
#include <boost/test/unit_test.hpp>

#include "endpoint/connection_stream.hpp"
#include "endpoint/http_request_exception.hpp"
#include "endpoint/model_request.hpp"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

// Move-only by design: one connection, one owner.
static_assert(std::is_move_constructible_v<endpoint::connection_stream>);
static_assert(std::is_move_assignable_v<endpoint::connection_stream>);
static_assert(!std::is_copy_constructible_v<endpoint::connection_stream>);
static_assert(!std::is_copy_assignable_v<endpoint::connection_stream>);

// --- empty state -------------------------------------------------------------

BOOST_AUTO_TEST_CASE(default_and_null_adoption_are_empty)
{
    endpoint::connection_stream stream;
    BOOST_CHECK(stream.empty());

    // A null pointer adopted at construction degenerates to the same state.
    endpoint::connection_stream from_null_tls{
        std::unique_ptr<endpoint::https_stream>{}};
    endpoint::connection_stream from_null_plain{
        std::unique_ptr<endpoint::http_stream>{}};
    BOOST_CHECK(from_null_tls.empty());
    BOOST_CHECK(from_null_plain.empty());

    // Moved-from is empty; the move target is not.
    endpoint::connection_stream moved_to{std::move(from_null_tls)};
    BOOST_CHECK(moved_to.empty());

    // close() on an empty stream is a harmless no-op.
    stream.close();
}

BOOST_AUTO_TEST_CASE(moved_from_connected_stream_is_empty)
{
    // The move operations park the SOURCE back in the empty state, so a
    // moved-from stream that held a real connection reports empty and its
    // guards fire — instead of dereferencing the moved-null pointer a
    // defaulted move would have left inside the variant.
    asio::io_context io;
    endpoint::connection_stream source{
        std::make_unique<endpoint::http_stream>(io.get_executor())};
    BOOST_REQUIRE(!source.empty());

    endpoint::connection_stream move_constructed{std::move(source)};
    BOOST_CHECK(!move_constructed.empty());
    BOOST_CHECK(source.empty());
    BOOST_CHECK_THROW(
        source.expires_after(std::chrono::seconds(1)), HttpRequestException);
    source.close();   // empty no-op, must not crash

    // Move assignment lands its source in the same state.
    endpoint::connection_stream move_assigned;
    move_assigned = std::move(move_constructed);
    BOOST_CHECK(!move_assigned.empty());
    BOOST_CHECK(move_constructed.empty());
    move_constructed.close();
}

BOOST_AUTO_TEST_CASE(empty_state_sync_operations_throw)
{
    endpoint::connection_stream stream;
    BOOST_CHECK_THROW(stream.is_tls(), HttpRequestException);
    BOOST_CHECK_THROW(
        stream.expires_after(std::chrono::seconds(1)), HttpRequestException);
    BOOST_CHECK_THROW(stream.expires_never(), HttpRequestException);
}

BOOST_AUTO_TEST_CASE(empty_state_async_operations_throw_when_awaited)
{
    // The awaitable members run their guard inside the coroutine body, so
    // the exception surfaces through the co_await — here via use_future.
    asio::io_context io;
    auto result = asio::co_spawn(
        io,
        []() -> asio::awaitable<void> {
            endpoint::connection_stream stream;
            http::request<http::string_body> request{http::verb::get, "/", 11};
            co_await stream.write(request);
        },
        asio::use_future);
    io.run();
    try {
        result.get();
        BOOST_FAIL("expected HttpRequestException for an empty stream");
    } catch (const HttpRequestException& error) {
        BOOST_CHECK(error.stage() == HttpRequestException::Stage::Unknown);
    }
}

// --- flavour reporting ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(adoption_reports_the_flavour)
{
    asio::io_context io;
    // An unconnected stream is enough — is_tls() only inspects the held
    // alternative, no I/O happens.
    endpoint::connection_stream tls_stream{std::make_unique<endpoint::https_stream>(
        io.get_executor(), endpoint::get_global_ssl_context())};
    BOOST_CHECK(!tls_stream.empty());
    BOOST_CHECK(tls_stream.is_tls());

    // Manually connect a plain stream and adopt it.
    tcp::acceptor acceptor(
        io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();
    std::thread server([&acceptor] {
        asio::io_context server_io;
        tcp::socket socket(acceptor.get_executor());
        acceptor.accept(socket);
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    });

    auto connect = asio::co_spawn(
        io,
        [port]() -> asio::awaitable<endpoint::connection_stream> {
            auto resolved = co_await endpoint::create_connection_stream(
                endpoint::ResolvedEndpoint{
                    .host = "127.0.0.1",
                    .port = std::to_string(port),
                    .tls = false});
            co_return resolved;
        },
        asio::use_future);
    io.run();
    endpoint::connection_stream plain_stream = connect.get();
    server.join();

    BOOST_CHECK(!plain_stream.empty());
    BOOST_CHECK(!plain_stream.is_tls());
    plain_stream.close();
}

// --- one full exchange through the facade --------------------------------------

BOOST_AUTO_TEST_CASE(plain_roundtrip_write_read_shutdown)
{
    // One-shot server: read one request, write one fixed response, then wait
    // for the client's shutdown before closing.
    asio::io_context server_io;
    tcp::acceptor acceptor(
        server_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = acceptor.local_endpoint().port();
    std::thread server([&] {
        tcp::socket socket(server_io);
        acceptor.accept(socket);
        boost::system::error_code ignored;
        http::request<http::string_body> request;
        boost::beast::flat_buffer buffer;
        http::read(socket, buffer, request, ignored);
        http::response<http::string_body> response{http::status::ok, 11};
        response.set(http::field::server, "loopback");
        response.body() = "facade-roundtrip";
        response.prepare_payload();
        http::write(socket, response, ignored);
        // Drain until the client half-closes, so the write is never lost.
        char sink[512];
        socket.read_some(asio::buffer(sink), ignored);
        boost::system::error_code close_ignored;
        socket.close(close_ignored);
    });

    asio::io_context io;
    auto exchange = asio::co_spawn(
        io,
        [port]() -> asio::awaitable<std::string> {
            auto stream = co_await endpoint::create_connection_stream(
                endpoint::ResolvedEndpoint{
                    .host = "127.0.0.1",
                    .port = std::to_string(port),
                    .tls = false});

            stream.expires_after(std::chrono::seconds(5));
            http::request<http::string_body> request{
                http::verb::get, "/", 11};
            request.set(http::field::host, "localhost");
            co_await stream.write(request);

            boost::beast::flat_buffer buffer;
            http::response_parser<http::string_body> parser;
            co_await stream.read(buffer, parser);

            std::string body = parser.get().body();
            co_await stream.shutdown();
            co_return body;
        },
        asio::use_future);
    io.run();
    const std::string body = exchange.get();
    server.join();

    BOOST_CHECK_EQUAL(body, "facade-roundtrip");
}
