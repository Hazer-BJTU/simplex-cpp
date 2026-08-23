// Deterministic, offline tests for complete_once — the whole-exchange
// convenience: the Connect-stage wrapping of connect failures (including the
// transport error code), the log-only exception policy of the spawned producer
// task, and the inline consumer drain to the assembled model_io::MessageItem.
// A plain loopback OneShotServer accepts the connect (the exchange never
// touches the socket — the fake drivers talk to the handler only), and a
// released loopback port supplies the refused-connect case. Every test runs
// its io_context to quiescence, so nothing here hangs on network timing.
#define BOOST_TEST_MODULE complete_once
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "endpoint/complete.hpp"
#include "endpoint/http_request_exception.hpp"

#include "loopback_server.hpp"

namespace asio = boost::asio;
namespace http = boost::beast::http;

// --- fakes: the minimal reader/handler pair complete_once needs ---------------

// Every framed event decodes to a terminal delta: one put() = done.
struct FakeDelta {
    bool terminal = false;
};
using FakeHandlerBase = endpoint::SSEResponseHandler<FakeDelta>;

struct TerminalHandler : FakeHandlerBase {
    using FakeHandlerBase::FakeHandlerBase;
    FakeDelta _handle_message(std::span<const LineInfo>) override {
        return FakeDelta{true};
    }
};

// Records the consume-side visits so the tests can pin what actually ran;
// _assemble marks the returned item so the happy path proves complete_once
// hands back the READER's assembled response, not some other value.
struct FakeReader : endpoint::ModelResponseReader<FakeDelta> {
    explicit FakeReader(std::shared_ptr<TerminalHandler> handler)
        : endpoint::ModelResponseReader<FakeDelta>(std::move(handler)) {}

    void _accumulate(const FakeDelta&) override { accumulated = true; }
    bool _is_terminal(const FakeDelta& delta) const override {
        return delta.terminal;
    }
    void _assemble() override {
        assembled = true;
        item.role = "assembled";
    }
    const model_io::MessageItem& response() const override { return item; }

    bool accumulated = false;
    bool assembled = false;
    model_io::MessageItem item{};
};

using Request = endpoint::ModelRequestInterpreter::HttpRequest;

// --- fake drivers ---------------------------------------------------------------

// Happy path: one terminal event through the handler's put side. The connected
// stream is never touched, so the server never needs to speak.
struct OneEventDriver {
    boost::asio::awaitable<void> operator()(
        std::shared_ptr<FakeHandlerBase> handler,
        endpoint::connection_stream,
        Request) const {
        co_await handler->put("data: hello\n\n");
    }
};

// Module-lifecycle failure: must be logged inside the spawn and NOT escape
// the exchange. put("") hands over without delivering anything, so the
// coroutine body is a plain throw afterwards.
struct FailingDriver {
    boost::asio::awaitable<void> operator()(
        std::shared_ptr<FakeHandlerBase> handler,
        endpoint::connection_stream,
        Request) const {
        co_await handler->put("");
        throw HttpRequestException(
            HttpRequestException::Stage::Write,
            "deliberate write failure", {}, "POST", "/v1/test", "127.0.0.1");
    }
};

// A non-http std::exception: exercises the spawn's wrap_request_failure
// fallback (Stage::Unknown with the request context kept for the log line).
struct StrayDriver {
    boost::asio::awaitable<void> operator()(
        std::shared_ptr<FakeHandlerBase> handler,
        endpoint::connection_stream,
        Request) const {
        co_await handler->put("");
        throw std::runtime_error("deliberate stray failure");
    }
};

// Sentinel for the connect-failure test: the connect throws before any spawn,
// so this driver must never even be copied into a running exchange.
struct UninvokedDriver {
    bool* invoked = nullptr;
    boost::asio::awaitable<void> operator()(
        std::shared_ptr<FakeHandlerBase>,
        endpoint::connection_stream,
        Request) const {
        *invoked = true;
        co_return;
    }
};

// --- harness --------------------------------------------------------------------

// Spawn complete_once on a fresh io_context and run it to completion; the
// future rethrows whatever the coroutine surfaced.
template<typename Driver>
std::future<model_io::MessageItem> run_exchange(
    asio::io_context& io,
    const endpoint::ResolvedEndpoint& where,
    std::shared_ptr<FakeReader> reader,
    Driver driver)
{
    Request request{http::verb::post, "/v1/complete", 11};
    auto operation = endpoint::complete_once<FakeDelta>(
        io.get_executor(), where, std::move(request), std::move(reader),
        std::move(driver));
    return asio::co_spawn(io, std::move(operation), asio::use_future);
}

// A loopback port that nobody listens on: bind, read the port, release.
static unsigned short refused_port() {
    asio::io_context io;
    tcp::acceptor probe(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = probe.local_endpoint().port();
    probe.close();
    return port;
}

// --- tests ----------------------------------------------------------------------

// A refused connect surfaces to the caller as HttpRequestException{Connect}
// with the endpoint context AND the transport error code preserved — the
// category (asio.system / netdb / ssl) is what callers classify by.
BOOST_AUTO_TEST_CASE(connect_failure_is_wrapped_as_stage_connect) {
    asio::io_context io;
    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));

    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(refused_port()),
        .target = "/v1/complete",
        .tls = false};

    bool invoked = false;
    auto result = run_exchange(
        io, where, reader, UninvokedDriver{&invoked});
    io.run();

    // One pass: get() consumes the future, so the checks live in the catch.
    try {
        result.get();
        BOOST_FAIL("connect failure did not propagate");
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::Connect);
        BOOST_CHECK_EQUAL(e.host(), "127.0.0.1");
        BOOST_CHECK_EQUAL(e.target(), "/v1/complete");
        // THE point of the connect wrap: the system_error's code survives.
        BOOST_CHECK(e.error_code() == asio::error::connection_refused);
    }
    // The failure precedes the spawn: no exchange ran, nothing accumulated.
    BOOST_CHECK(!invoked);
    BOOST_CHECK(!reader->accumulated);
    BOOST_CHECK(!reader->assembled);
}

// The happy path: connect, one terminal event through the spawned producer,
// the inline consumer assembles, and complete_once returns the READER's item.
BOOST_AUTO_TEST_CASE(happy_exchange_returns_the_assembled_item) {
    // Accept the connect and close: the fake driver never touches the socket.
    loopback::OneShotServer server([](tcp::socket&) {});

    asio::io_context io;
    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));

    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    auto result = run_exchange(io, where, reader, OneEventDriver{});
    io.run();

    const auto item = result.get();
    BOOST_CHECK(reader->accumulated);
    BOOST_CHECK(reader->assembled);
    // _assemble() wrote this role — the returned item IS the reader's
    // assembled response.
    BOOST_CHECK_EQUAL(item.role, "assembled");
    server.join();
}

// A driver failure never escapes the exchange: pump has already finished the
// handler ERROR-side, so the outcome arrives through the consumer — the
// logged HttpRequestException stays inside the spawn, and complete_once
// returns the reader's unassembled default.
BOOST_AUTO_TEST_CASE(driver_http_failure_is_log_only) {
    loopback::OneShotServer server([](tcp::socket&) {});

    asio::io_context io;
    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));

    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    auto result = run_exchange(io, where, reader, FailingDriver{});
    io.run();

    const auto item = result.get();   // returns; nothing propagates
    BOOST_CHECK(!reader->accumulated);
    BOOST_CHECK(!reader->assembled);
    BOOST_CHECK_NE(item.role, "assembled");
    server.join();
}

// The defensive-depth branch: a stray std::exception out of the driver is
// folded into HttpRequestException{Unknown} (keeping the request context for
// the log line) rather than escaping the detached spawn.
BOOST_AUTO_TEST_CASE(driver_stray_std_failure_is_wrapped_and_log_only) {
    loopback::OneShotServer server([](tcp::socket&) {});

    asio::io_context io;
    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));

    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    auto result = run_exchange(io, where, reader, StrayDriver{});
    io.run();

    const auto item = result.get();   // returns; nothing propagates
    BOOST_CHECK(!reader->accumulated);
    BOOST_CHECK(!reader->assembled);
    BOOST_CHECK_NE(item.role, "assembled");
    server.join();
}
