// Deterministic, offline unit tests for endpoint::http_request — the bounded,
// single-body counterpart of sse_request — over a plain stream. A one-shot
// loopback server replies with fixed complete responses; both the direct
// (awaitable-response) overload and the AsyncResponseHandler overload are
// driven end-to-end on a single io_context.
#define BOOST_TEST_MODULE HttpRequestTests
#include <boost/test/unit_test.hpp>

#include "endpoint/http_request_exception.hpp"
#include "endpoint/model_request.hpp"
#include "endpoint/request.hpp"
#include "loopback_server.hpp"

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace asio = boost::asio;
namespace http = boost::beast::http;

using endpoint::AsyncResponseHandler;
using loopback::OneShotServer;

// A minimal generic AsyncResponseHandler: one complete body in, the same body
// out through a single-slot channel — enough to exercise the handler
// overload's put/get contract with real producer/consumer suspension, however
// the scheduler interleaves the two coroutines.
class BodyHandler final : public AsyncResponseHandler<std::string> {
public:
    explicit BodyHandler(asio::any_io_executor executor)
        : _channel(executor, 1)
    {}

    boost::asio::awaitable<void> put(std::string_view payload) override {
        boost::system::error_code ec;
        co_await _channel.async_send(
            {}, std::string(payload),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) throw std::runtime_error("body channel send failed");
    }

    boost::asio::awaitable<std::string> get() override {
        boost::system::error_code ec;
        std::string body = co_await _channel.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        if (ec) throw std::runtime_error("body channel receive failed");
        co_return body;
    }

    // Test-side control: unblock a get() that can never be satisfied because
    // the producer side failed.
    void close() { _channel.close(); }

private:
    asio::experimental::channel<void(boost::system::error_code, std::string)>
        _channel;
};

// --- driver-as-callable contract ---------------------------------------------
// The layer above (full-flow + retry) takes the request driver as a type
// parameter constrained by RequestDriver. Pin the concept to the functor
// form — base and derived handler — so a signature drift fails here, loudly,
// instead of silently at the wrapper.
static_assert(endpoint::RequestDriver<
              endpoint::HttpRequestDriver<std::string>,
              endpoint::AsyncResponseHandler<std::string>>);
static_assert(endpoint::RequestDriver<
              endpoint::HttpRequestDriver<std::string>,
              BodyHandler>);
// The direct (response-returning) overload takes no handler, so it is not
// nameable as a driver at all. And the raw pointer to the handler overload is
// not one either — its defaulted read_timeout_sec is unreachable through a
// pointer to a function template specialization, which is exactly why
// HttpRequestDriver exists.
static_assert(!endpoint::RequestDriver<
              decltype(&endpoint::http_request<std::string>),
              endpoint::AsyncResponseHandler<std::string>>);

// --- client-side drivers -----------------------------------------------------

struct DirectOutcome {
    std::optional<http::response<http::string_body>> response;
    std::optional<HttpRequestException::Stage> stage;
    std::optional<std::string> error;
    bool timed_out = false;
};

// POST a small JSON body at the loopback server via the direct overload.
static DirectOutcome run_direct(
    unsigned short port,
    std::size_t read_timeout_sec = endpoint::DEFAULT_HTTP_READ_TIMEOUT_SEC)
{
    asio::io_context io;
    DirectOutcome outcome;

    asio::co_spawn(
        io,
        [&outcome, port, read_timeout_sec]() mutable -> asio::awaitable<void> {
            try {
                auto stream = co_await endpoint::create_connection_stream(
                    endpoint::ResolvedEndpoint{
                        .host = "127.0.0.1",
                        .port = std::to_string(port),
                        .tls = false});
                http::request<http::string_body> request{
                    http::verb::post, "/v1/responses", 11};
                request.set(http::field::host, "localhost");
                request.set(http::field::content_type, "application/json");
                request.body() = "{\"model\":\"m\"}";
                request.prepare_payload();
                outcome.response = co_await endpoint::http_request(
                    std::move(stream), std::move(request), read_timeout_sec);
            } catch (const HttpRequestTimeoutException& error) {
                // Most specific first: still an HttpRequestException below.
                outcome.timed_out = true;
                outcome.stage = error.stage();
                outcome.error = error.what();
            } catch (const HttpRequestException& error) {
                outcome.stage = error.stage();
                outcome.error = error.what();
            } catch (const boost::system::system_error& error) {
                outcome.error = error.what();
            }
            co_return;
        },
        asio::detached);

    io.run();
    return outcome;
}

struct HandlerOutcome {
    std::optional<std::string> body;
    std::optional<HttpRequestException::Stage> stage;
    std::optional<unsigned> status;
    std::optional<std::string> error;
};

// Same exchange through any RequestDriver: the producer calls the driver
// with the uniform three-argument shape — no per-driver branches — while the
// consumer drains get() once.
template<endpoint::RequestDriver<BodyHandler> Driver>
static HandlerOutcome run_with_handler(unsigned short port, Driver driver) {
    asio::io_context io;
    HandlerOutcome outcome;
    auto handler = std::make_shared<BodyHandler>(io.get_executor());

    asio::co_spawn(
        io,
        [&outcome, handler, port, driver]() mutable -> asio::awaitable<void> {
            try {
                auto stream = co_await endpoint::create_connection_stream(
                    endpoint::ResolvedEndpoint{
                        .host = "127.0.0.1",
                        .port = std::to_string(port),
                        .tls = false});
                http::request<http::string_body> request{
                    http::verb::post, "/v1/responses", 11};
                request.set(http::field::host, "localhost");
                request.body() = "{\"model\":\"m\"}";
                request.prepare_payload();
                co_await driver(handler, std::move(stream), std::move(request));
            } catch (const HttpRequestException& error) {
                outcome.stage = error.stage();
                outcome.status = error.status();
                outcome.error = error.what();
                // No product will ever arrive; unblock the consumer.
                handler->close();
            } catch (const boost::system::system_error& error) {
                outcome.error = error.what();
                handler->close();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&outcome, handler]() mutable -> asio::awaitable<void> {
            try {
                outcome.body = co_await handler->get();
            } catch (const std::exception& error) {
                if (!outcome.error) outcome.error = error.what();
            }
        },
        asio::detached);

    io.run();
    return outcome;
}

// --- tests -------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(direct_returns_the_complete_response)
{
    OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(
            socket,
            http::status::ok,
            "{\"id\":\"resp_1\"}",
            {{"x-request-id", "42"}});
    });
    const unsigned short port = server.wait_listening();

    DirectOutcome outcome = run_direct(port);
    server.join();

    BOOST_CHECK(!outcome.error.has_value());
    if (outcome.error)
        BOOST_TEST_MESSAGE("error: " << *outcome.error);
    BOOST_REQUIRE(outcome.response.has_value());
    BOOST_CHECK(outcome.response->result() == http::status::ok);
    BOOST_CHECK_EQUAL(outcome.response->body(), "{\"id\":\"resp_1\"}");
    // Headers survive too — rate limits, request ids, ... stay available.
    BOOST_CHECK_EQUAL((*outcome.response)["x-request-id"], "42");
}

BOOST_AUTO_TEST_CASE(direct_preserves_error_status_and_body)
{
    // The no-gating contract: a non-2xx response is a result, not a throw —
    // the provider's error payload is exactly what the caller needs.
    OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(
            socket, http::status::unauthorized, "{\"error\":\"bad key\"}");
    });
    const unsigned short port = server.wait_listening();

    DirectOutcome outcome = run_direct(port);
    server.join();

    BOOST_REQUIRE(outcome.response.has_value());
    BOOST_CHECK(outcome.response->result() == http::status::unauthorized);
    BOOST_CHECK_EQUAL(outcome.response->body(), "{\"error\":\"bad key\"}");
}

BOOST_AUTO_TEST_CASE(handler_delivers_the_body_via_get)
{
    OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, "payload");
    });
    const unsigned short port = server.wait_listening();

    HandlerOutcome outcome = run_with_handler(
        port, endpoint::HttpRequestDriver<std::string>{});
    server.join();

    BOOST_CHECK(!outcome.error.has_value());
    if (outcome.error)
        BOOST_TEST_MESSAGE("error: " << *outcome.error);
    BOOST_REQUIRE(outcome.body.has_value());
    BOOST_CHECK_EQUAL(*outcome.body, "payload");
}

BOOST_AUTO_TEST_CASE(handler_rejects_non_ok_status)
{
    // The handler path gates on the status, unlike the direct overload: a
    // non-200 never becomes a Product — it surfaces as a HandleResponse
    // failure carrying the status code.
    OneShotServer server([](tcp::socket& socket) {
        loopback::serve_fixed_response(
            socket, http::status::too_many_requests, "{\"error\":\"slow down\"}");
    });
    const unsigned short port = server.wait_listening();

    HandlerOutcome outcome = run_with_handler(
        port, endpoint::HttpRequestDriver<std::string>{});
    server.join();

    BOOST_REQUIRE(outcome.stage.has_value());
    BOOST_CHECK(*outcome.stage == HttpRequestException::Stage::HandleResponse);
    BOOST_REQUIRE(outcome.status.has_value());
    BOOST_CHECK_EQUAL(*outcome.status, 429u);
    // what() is the full rendering now: the rejection phrase plus the status
    // and request context a bare catch would otherwise lose.
    BOOST_CHECK(outcome.error->find("HTTP request rejected") != std::string::npos);
    BOOST_CHECK(outcome.error->find("HTTP status 429") != std::string::npos);
    BOOST_CHECK(!outcome.body.has_value());
}

BOOST_AUTO_TEST_CASE(handler_driver_timeout_is_baked_in)
{
    // The deadline is a constructor argument of the driver functor, and it
    // reaches the bounded exchange: a silent server plus a 1s baked-in
    // deadline ends in the read-timeout exception.
    OneShotServer server([](tcp::socket& socket) {
        loopback::serve_silent_delay(socket, std::chrono::seconds(2));
    });
    const unsigned short port = server.wait_listening();

    HandlerOutcome outcome = run_with_handler(
        port, endpoint::HttpRequestDriver<std::string>{1});
    server.join();

    BOOST_REQUIRE(outcome.stage.has_value());
    BOOST_CHECK(*outcome.stage == HttpRequestException::Stage::Read);
    BOOST_REQUIRE(outcome.error.has_value());
    BOOST_CHECK(outcome.error->find("after 1s") != std::string::npos);
    BOOST_CHECK(!outcome.body.has_value());
}

BOOST_AUTO_TEST_CASE(transport_failure_surfaces_as_read_stage_error)
{
    OneShotServer server([](tcp::socket& socket) {
        loopback::serve_close_without_response(socket);
    });
    const unsigned short port = server.wait_listening();

    DirectOutcome outcome = run_direct(port);
    server.join();

    BOOST_REQUIRE(outcome.stage.has_value());
    BOOST_CHECK(*outcome.stage == HttpRequestException::Stage::Read);
    BOOST_CHECK(!outcome.timed_out);
    BOOST_CHECK(!outcome.response.has_value());
}

BOOST_AUTO_TEST_CASE(read_timeout_throws_the_specific_exception)
{
    // The server reads the request, then holds the connection silent for
    // longer than the client's 1s read deadline — the slow-backend case.
    OneShotServer server([](tcp::socket& socket) {
        loopback::serve_silent_delay(socket, std::chrono::seconds(2));
    });
    const unsigned short port = server.wait_listening();

    DirectOutcome outcome = run_direct(port, /*read_timeout_sec=*/1);
    server.join();

    BOOST_CHECK(outcome.timed_out);
    BOOST_REQUIRE(outcome.stage.has_value());
    BOOST_CHECK(*outcome.stage == HttpRequestException::Stage::Read);
    BOOST_CHECK(!outcome.response.has_value());
    if (outcome.error)
        BOOST_TEST_MESSAGE("timeout error: " << *outcome.error);
}

BOOST_AUTO_TEST_CASE(empty_and_null_arguments_are_rejected)
{
    asio::io_context io;
    http::request<http::string_body> request{http::verb::get, "/events", 11};
    request.set(http::field::host, "localhost");

    // An empty connection_stream (no connection) is rejected by the facade's
    // own guard — the same HttpRequestException{Unknown} the old null-pointer
    // check produced.
    auto empty_stream = asio::co_spawn(
        io,
        endpoint::http_request(endpoint::connection_stream{}, request),
        asio::use_future);
    io.run();
    try {
        empty_stream.get();
        BOOST_FAIL("expected HttpRequestException for an empty stream");
    } catch (const HttpRequestException& error) {
        BOOST_CHECK(error.stage() == HttpRequestException::Stage::Unknown);
    }

    io.restart();
    auto null_handler = asio::co_spawn(
        io,
        endpoint::http_request<std::string>(
            nullptr, endpoint::connection_stream{}, request),
        asio::use_future);
    io.run();
    try {
        null_handler.get();
        BOOST_FAIL("expected HttpRequestException for a null handler");
    } catch (const HttpRequestException& error) {
        BOOST_CHECK(error.stage() == HttpRequestException::Stage::Unknown);
    }
}
