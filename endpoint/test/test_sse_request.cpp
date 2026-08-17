// Deterministic, offline unit tests for endpoint::sse_request over a plain
// (non-TLS) stream: a one-shot loopback HTTP server serves a fixed SSE body,
// and the templated pump is driven end-to-end — producer sse_request plus a
// consumer get() loop — on a single io_context, exactly as the module is meant
// to be used. The TLS flavour of the same flow stays covered by the manual
// sse_request_e2e executable (loopback + self-signed cert).
#define BOOST_TEST_MODULE SSERequestTests
#include <boost/test/unit_test.hpp>

#include "endpoint/https_stream.hpp"
#include "endpoint/http_request_exception.hpp"
#include "endpoint/request.hpp"
#include "loopback_server.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace asio = boost::asio;
namespace http = boost::beast::http;

using endpoint::SSEAborted;
using endpoint::SSEHandlerState;
using endpoint::SSEResponseHandler;
using loopback::OneShotServer;

// An SSE field line, matching SSEResponseHandler<...>::LineInfo.
using Field = std::pair<std::string, std::string>;

// A handler whose Product is simply the verbatim field list of each event.
class FieldHandler final : public SSEResponseHandler<std::vector<Field>> {
public:
    using SSEResponseHandler<std::vector<Field>>::SSEResponseHandler;

    std::vector<Field> _handle_message(std::span<const Field> message) override {
        return std::vector<Field>(message.begin(), message.end());
    }
};

static std::string data_value(const std::vector<Field>& event) {
    for (const auto& [field, value] : event)
        if (field == "data") return value;
    return {};
}

// --- client-side driver ------------------------------------------------------

struct Exchange {
    std::vector<std::vector<Field>> events;
    std::optional<HttpRequestException::Stage> put_stage;
    std::optional<std::string> put_error;
    std::optional<SSEHandlerState> consumer_end;
};

// Connect a plain stream to the loopback server, drive sse_request as the
// producer and drain get() as the consumer on one io_context, and return what
// each side observed. The handler channel is rendezvous, so the two coroutines
// interleave through the scheduler just like the real pipeline does.
static Exchange run_exchange(unsigned short port) {
    asio::io_context io;
    Exchange results;
    auto handler = std::make_shared<FieldHandler>(io.get_executor());

    asio::co_spawn(
        io,
        [&results, handler, port]() mutable -> asio::awaitable<void> {
            try {
                auto stream = co_await endpoint::create_http_connection_stream(
                    "127.0.0.1", std::to_string(port));
                http::request<http::string_body> request{http::verb::get, "/events", 11};
                request.set(http::field::host, "localhost");
                request.set(http::field::accept, "text/event-stream");
                co_await endpoint::sse_request<std::vector<Field>>(
                    handler, std::move(stream), std::move(request));
            } catch (const HttpRequestException& error) {
                results.put_stage = error.stage();
                results.put_error = error.what();
            } catch (const boost::system::system_error& error) {
                results.put_error = error.what();
            }
            handler->finish(SSEHandlerState::DONE);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&results, handler]() mutable -> asio::awaitable<void> {
            try {
                for (;;) {
                    results.events.push_back(co_await handler->get());
                }
            } catch (const SSEAborted& aborted) {
                results.consumer_end = aborted.state();
            }
        },
        asio::detached);

    io.run();
    return results;
}

// --- tests -------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(delivers_events_over_plain_http)
{
    OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(
            socket, http::status::ok, "data: alpha\n\ndata: beta\n\n");
    });
    const unsigned short port = server.wait_listening();

    Exchange results = run_exchange(port);
    server.join();

    BOOST_CHECK(!results.put_error.has_value());
    if (results.put_error)
        BOOST_TEST_MESSAGE("put error: " << *results.put_error);
    BOOST_REQUIRE_EQUAL(results.events.size(), 2u);
    BOOST_CHECK_EQUAL(data_value(results.events[0]), "alpha");
    BOOST_CHECK_EQUAL(data_value(results.events[1]), "beta");
    BOOST_REQUIRE(results.consumer_end.has_value());
    BOOST_CHECK(*results.consumer_end == SSEHandlerState::DONE);
}

BOOST_AUTO_TEST_CASE(rejected_status_surfaces_as_handle_response_error)
{
    OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(
            socket, http::status::internal_server_error, "data: ignored\n\n");
    });
    const unsigned short port = server.wait_listening();

    Exchange results = run_exchange(port);
    server.join();

    BOOST_REQUIRE(results.put_stage.has_value());
    BOOST_CHECK(*results.put_stage == HttpRequestException::Stage::HandleResponse);
    BOOST_CHECK(results.events.empty());
    BOOST_REQUIRE(results.consumer_end.has_value());
    BOOST_CHECK(*results.consumer_end == SSEHandlerState::DONE);
}

BOOST_AUTO_TEST_CASE(null_arguments_are_rejected)
{
    asio::io_context io;
    auto handler = std::make_shared<FieldHandler>(io.get_executor());
    http::request<http::string_body> request{http::verb::get, "/events", 11};
    request.set(http::field::host, "localhost");

    // A nullptr stream cannot deduce the Stream template parameter, so both
    // flavours of sse_request's explicit-argument form are exercised here.
    auto null_stream = asio::co_spawn(
        io,
        endpoint::sse_request<std::vector<Field>, endpoint::http_stream>(
            handler, nullptr, request),
        asio::use_future);
    io.run();
    try {
        null_stream.get();
        BOOST_FAIL("expected HttpRequestException for a null stream");
    } catch (const HttpRequestException& error) {
        BOOST_CHECK(error.stage() == HttpRequestException::Stage::Unknown);
    }

    io.restart();
    auto null_handler = asio::co_spawn(
        io,
        endpoint::sse_request<std::vector<Field>, endpoint::https_stream>(
            nullptr,
            std::make_unique<endpoint::https_stream>(
                io.get_executor(), endpoint::get_global_ssl_context()),
            request),
        asio::use_future);
    io.run();
    try {
        null_handler.get();
        BOOST_FAIL("expected HttpRequestException for a null handler");
    } catch (const HttpRequestException& error) {
        BOOST_CHECK(error.stage() == HttpRequestException::Stage::Unknown);
    }
}
