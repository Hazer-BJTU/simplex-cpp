// Deterministic, offline tests for complete_once and the complete functor —
// the whole-exchange conveniences: the Connect-stage wrapping of connect
// failures (including the transport error code), the log-only exception
// policy of the spawned producer task, the inline consumer drain to the
// assembled model_io::MessageItem, the reader's post-mortem end states, and
// the complete functor's plain-retry classification. A loopback server
// accepts the connect (the exchange never touches the socket — the fake
// drivers talk to the handler only), and a released loopback port supplies
// the refused-connect case. Every test runs its io_context to quiescence, so
// nothing here hangs on network timing.
#define BOOST_TEST_MODULE complete_once
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
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

    // The subclass half of the reuse reset: this layer's accumulators wiped
    // on top of the base's stream/handler rewind.
    void clear() override {
        endpoint::ModelResponseReader<FakeDelta>::clear();
        accumulated = false;
        assembled = false;
        item = model_io::MessageItem{};
    }

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

// Exposes complete's protected classification for the table test below.
struct RecoverabilityProbe : endpoint::complete {
    using endpoint::complete::complete;
    bool recoverable(const HttpRequestException& failure) noexcept {
        return _recoverable(failure);
    }
};

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
    // The post-mortem the retry layer trusts: the terminal event ended it.
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Completed);
    BOOST_CHECK(!reader->end_error());
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
    // Post-mortem: Faulted, and the producer exception complete_once only
    // logged is still reachable for classification — rethrown, it is the
    // original rich failure (stage and request context intact).
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Faulted);
    BOOST_REQUIRE(reader->end_error());
    try {
        std::rethrow_exception(reader->end_error());
        BOOST_FAIL("end_error() did not rethrow");
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::Write);
        BOOST_CHECK_EQUAL(e.target(), "/v1/test");
    }
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

// --- reader end states (the post-mortem complete() classifies on) ---------------

// abort() with no next() in flight must still surface Aborted: the recorded
// marker, not an SSEAborted nobody will observe, is what end_state() reads.
BOOST_AUTO_TEST_CASE(consumer_abort_without_next_is_reported_aborted) {
    asio::io_context io;
    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));

    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Streaming);

    reader->abort();

    BOOST_CHECK(reader->finished());
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Aborted);
    BOOST_CHECK(!reader->end_error());
    // The stream is idempotently over: next() short-circuits to nullopt.
    auto drained = asio::co_spawn(
        io, reader->next(), asio::use_future);
    io.run();
    BOOST_CHECK(drained.get() == std::nullopt);
    // A late abort() after a completed stream must not overwrite the state.
    reader->abort();
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Aborted);
}

// A hook fault propagates out of consume() (caller's bug) AND leaves the
// post-mortem Faulted — a dying monitor cannot masquerade as Completed.
BOOST_AUTO_TEST_CASE(hook_fault_marks_the_end_state_faulted) {
    loopback::OneShotServer server([](tcp::socket&) {});

    asio::io_context io;
    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));
    reader->add_hook([](const FakeDelta&) { throw std::runtime_error("hook bug"); });

    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    auto result = run_exchange(io, where, reader, OneEventDriver{});
    io.run();
    BOOST_CHECK_THROW(result.get(), std::runtime_error);   // passes through
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Faulted);
    // The terminal delta assembled BEFORE the hook ran; the record is final.
    BOOST_CHECK(reader->assembled);
    server.join();
}

// A stream that ends with NO terminal event and NO producer exception (the
// server closed early): Faulted with a null end_error — the truncated shape
// plain retry exists for.
BOOST_AUTO_TEST_CASE(eof_without_terminal_is_faulted_with_no_error) {
    loopback::OneShotServer server([](tcp::socket&) {});

    asio::io_context io;
    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));

    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    // EofDriver: returns cleanly without delivering anything.
    struct EofDriver {
        boost::asio::awaitable<void> operator()(
            std::shared_ptr<FakeHandlerBase>,
            endpoint::connection_stream,
            Request) const {
            co_return;
        }
    };
    auto result = run_exchange(io, where, reader, EofDriver{});
    io.run();

    const auto item = result.get();
    BOOST_CHECK(!reader->assembled);
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Faulted);
    BOOST_CHECK(!reader->end_error());
    (void)item;
    server.join();
}

// --- the default recoverability table --------------------------------------------

// Pins complete's default _recoverable verdicts: status first, then stage,
// then the connect error-code categories.
BOOST_AUTO_TEST_CASE(default_recoverability_table) {
    asio::io_context io;
    RecoverabilityProbe probe{io.get_executor()};

    using Stage = HttpRequestException::Stage;
    const auto failure = [](Stage stage, unsigned status = 0,
                            boost::system::error_code ec = {}) {
        return HttpRequestException(
            stage, "msg", ec, "POST", "/t", "h", status);
    };

    // Status-led: the provider answered.
    BOOST_CHECK(probe.recoverable(
        failure(Stage::HandleResponse, 429)));
    BOOST_CHECK(probe.recoverable(
        failure(Stage::HandleResponse, 408)));
    BOOST_CHECK(probe.recoverable(
        failure(Stage::HandleResponse, 503)));
    BOOST_CHECK(!probe.recoverable(
        failure(Stage::HandleResponse, 401)));
    BOOST_CHECK(!probe.recoverable(
        failure(Stage::HandleResponse, 400)));
    BOOST_CHECK(!probe.recoverable(
        failure(Stage::HandleResponse, 404)));

    // Stage-led (no status): mid-exchange transport is transient, our own
    // build bugs and the unclassifiable are not.
    BOOST_CHECK(probe.recoverable(failure(Stage::Write)));
    BOOST_CHECK(probe.recoverable(failure(Stage::Read)));
    BOOST_CHECK(probe.recoverable(
        failure(Stage::HandleResponse)));
    BOOST_CHECK(!probe.recoverable(
        failure(Stage::CreateRequest)));
    BOOST_CHECK(!probe.recoverable(failure(Stage::Unknown)));

    // The bounded driver's timeout flavour classifies as its Read stage.
    const HttpRequestTimeoutException timeout("read timed out");
    BOOST_CHECK(probe.recoverable(timeout));

    // Connect error-code categories.
    BOOST_CHECK(probe.recoverable(failure(
        Stage::Connect, 0, asio::error::connection_refused)));
    BOOST_CHECK(probe.recoverable(failure(
        Stage::Connect, 0, asio::error::connection_reset)));
    BOOST_CHECK(probe.recoverable(failure(
        Stage::Connect, 0, asio::error::timed_out)));
    BOOST_CHECK(probe.recoverable(failure(
        Stage::Connect, 0, boost::beast::error::timeout)));
    // Resolver: TRY_AGAIN is transient, authoritative not-found is config.
    BOOST_CHECK(probe.recoverable(failure(
        Stage::Connect, 0, asio::error::host_not_found_try_again)));
    BOOST_CHECK(!probe.recoverable(failure(
        Stage::Connect, 0, asio::error::host_not_found)));
    // TLS: a cut handshake may be a middlebox hiccup; anything else in the
    // SSL category (e.g. verification outcomes) is not retryable material.
    BOOST_CHECK(probe.recoverable(failure(
        Stage::Connect, 0, asio::ssl::error::stream_truncated)));
    BOOST_CHECK(!probe.recoverable(failure(
        Stage::Connect, 0,
        boost::system::error_code(2, asio::ssl::error::get_stream_category()))));
}

// --- complete: the plain-retry whole exchange --------------------------------------

// Accepts `count` consecutive connections and closes each immediately: the
// connect inside complete_once succeeds on every attempt; the fake drivers
// never touch the socket, so nothing else is needed server-side.
class AcceptAllServer {
public:
    explicit AcceptAllServer(int count)
        : _port_promise(std::make_shared<std::promise<unsigned short>>())
        , _done_promise(std::make_shared<std::promise<void>>())
        , _port(_port_promise->get_future())
        , _done(_done_promise->get_future())
        , _thread([this, count] {
              try {
                  asio::io_context io;
                  tcp::acceptor acceptor(
                      io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
                  _port_promise->set_value(acceptor.local_endpoint().port());
                  for (int i = 0; i < count; ++i) {
                      tcp::socket socket(io);   // accepted, then closed by scope
                      acceptor.accept(socket);
                  }
                  _done_promise->set_value();
              } catch (...) {
                  try {
                      _done_promise->set_exception(std::current_exception());
                  } catch (...) {}
              }
          }) {}

    unsigned short wait_listening() { return _port.get(); }
    void join() {
        _thread.join();
        _done.get();
    }

private:
    std::shared_ptr<std::promise<unsigned short>> _port_promise;
    std::shared_ptr<std::promise<void>> _done_promise;
    std::future<unsigned short> _port;
    std::future<void> _done;
    std::thread _thread;
};

// Drives one complete-functor exchange on a fresh io_context; rethrows what
// it surfaced. The completer is the CALLER'S object and must outlive io.run()
// (the coroutine holds `this`); the reader is created once and reused across
// attempts — complete's clear() rewinds it per attempt.
template<typename Driver>
std::future<model_io::MessageItem> run_retry(
    asio::io_context& io,
    endpoint::complete& completer,
    const endpoint::ResolvedEndpoint& where,
    std::shared_ptr<FakeReader> reader,
    Driver driver)
{
    Request request{http::verb::post, "/v1/complete", 11};
    auto operation = completer.operator()<FakeDelta>(
        where, std::move(request), std::move(reader), std::move(driver));
    return asio::co_spawn(io, std::move(operation), asio::use_future);
}

// Fails the first exchange with a mid-read transport fault (recoverable by
// the default table), completes on the second — the canonical plain-retry
// story: the SAME reader, cleared between attempts, whole response re-read.
// Succeeding on attempt 2 is itself the proof clear() worked: without the
// reset, attempt 1's end state would short-circuit every later next() and
// the loop could only exhaust or throw.
BOOST_AUTO_TEST_CASE(complete_retries_a_transient_fault_and_succeeds) {
    AcceptAllServer server(2);

    asio::io_context io;
    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    struct FlakyDriver {
        std::shared_ptr<int> invocations;
        boost::asio::awaitable<void> operator()(
            std::shared_ptr<FakeHandlerBase> handler,
            endpoint::connection_stream,
            Request) const {
            if (++*invocations == 1) {
                co_await handler->put("");
                throw HttpRequestException(
                    HttpRequestException::Stage::Read, "transient read fault");
            }
            co_await handler->put("data: again\n\n");
        }
    };

    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));
    // Fast backoffs for the test; budget = initial + 2 retries.
    endpoint::complete completer(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 2);
    const auto driver_calls = std::make_shared<int>(0);
    auto result = run_retry(io, completer, where, reader,
                             FlakyDriver{driver_calls});
    io.run();

    const auto item = result.get();
    BOOST_CHECK_EQUAL(*driver_calls, 2);   // the exchange ran twice
    BOOST_CHECK_EQUAL(item.role, "assembled");
    BOOST_CHECK(reader->assembled);        // ...on the reused reader
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Completed);
    server.join();
}

// A provider rejection the same request would draw again (401): no retry,
// the original failure propagates immediately with its context.
BOOST_AUTO_TEST_CASE(complete_does_not_retry_a_non_recoverable_rejection) {
    AcceptAllServer server(1);   // one exchange only: the 401 stops the loop

    asio::io_context io;
    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    struct RejectedDriver {
        std::shared_ptr<int> invocations;
        boost::asio::awaitable<void> operator()(
            std::shared_ptr<FakeHandlerBase> handler,
            endpoint::connection_stream,
            Request) const {
            ++*invocations;
            co_await handler->put("");
            throw HttpRequestException(
                HttpRequestException::Stage::HandleResponse,
                "unauthorized", {}, "POST", "/v1/complete", "127.0.0.1", 401);
        }
    };

    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));
    endpoint::complete completer(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 2);
    const auto calls = std::make_shared<int>(0);
    auto result = run_retry(io, completer, where, reader, RejectedDriver{calls});
    io.run();

    try {
        result.get();
        BOOST_FAIL("the 401 rejection did not propagate");
    } catch (const HttpRequestException& e) {
        BOOST_CHECK_EQUAL(e.status(), 401u);
    }
    BOOST_CHECK_EQUAL(*calls, 1);   // one exchange, no retry
    server.join();
}

// Every attempt ends truncated (server closes, no terminal event, no
// exception): the budget is spent re-reading, then the truncation wrap
// surfaces as the report.
BOOST_AUTO_TEST_CASE(complete_retries_truncation_to_the_budget_then_reports) {
    AcceptAllServer server(3);

    asio::io_context io;
    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    struct EofDriver {
        std::shared_ptr<int> invocations;
        boost::asio::awaitable<void> operator()(
            std::shared_ptr<FakeHandlerBase>,
            endpoint::connection_stream,
            Request) const {
            ++*invocations;
            co_return;   // clean producer exit, nothing delivered
        }
    };

    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));
    // Budget: initial + 2 retries = 3 exchanges, all truncated.
    endpoint::complete completer(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 2);
    const auto calls = std::make_shared<int>(0);
    auto result = run_retry(io, completer, where, reader, EofDriver{calls});
    io.run();

    try {
        result.get();
        BOOST_FAIL("the truncated stream did not surface");
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::Read);
        BOOST_CHECK_EQUAL(e.what(), "stream ended without the terminal event");
    }
    BOOST_CHECK_EQUAL(*calls, 3);   // the whole budget re-read
    server.join();
}

// A consumer abort is not a failure to retry — even a classification that
// calls everything recoverable must not outvote the caller's abort(). The
// abort is injected MID-attempt (a watchdog coroutine after 2ms of a 50ms
// idle exchange — the real shape, a deadline firing from outside):
// complete's loop-top clear() would wipe a pre-abort, so the abort must
// land inside the exchange itself.
BOOST_AUTO_TEST_CASE(complete_never_retries_a_consumer_abort) {
    AcceptAllServer server(1);   // the abort ends it after one connect

    asio::io_context io;
    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    // Keeps the exchange open (nothing delivered) long enough for the
    // watchdog's abort to land inside it.
    struct IdleDriver {
        std::shared_ptr<int> invocations;
        boost::asio::awaitable<void> operator()(
            std::shared_ptr<FakeHandlerBase>,
            endpoint::connection_stream,
            Request) const {
            ++*invocations;
            asio::steady_timer nap(co_await asio::this_coro::executor);
            nap.expires_after(std::chrono::milliseconds(50));
            co_await nap.async_wait(asio::use_awaitable);
        }
    };

    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));

    // The permissive verdict: everything recoverable — abort must still win.
    struct AlwaysRetryable : endpoint::complete {
        using endpoint::complete::complete;
        bool _recoverable(const HttpRequestException&) noexcept override {
            return true;
        }
    };
    AlwaysRetryable completer(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 2);

    const auto calls = std::make_shared<int>(0);
    Request request{http::verb::post, "/v1/complete", 11};
    auto operation = completer.operator()<FakeDelta>(
        where, std::move(request), reader, IdleDriver{calls});
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);

    // The watchdog: the consumer's own deadline, firing mid-exchange.
    asio::co_spawn(
        io,
        [reader]() -> asio::awaitable<void> {
            asio::steady_timer deadline(
                co_await asio::this_coro::executor);
            deadline.expires_after(std::chrono::milliseconds(2));
            co_await deadline.async_wait(asio::use_awaitable);
            reader->abort();
        },
        asio::detached);
    io.run();

    try {
        result.get();
        BOOST_FAIL("the consumer abort did not surface");
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::Unknown);
        BOOST_CHECK_EQUAL(
            e.what(), "stream aborted by the consumer before the terminal event");
    }
    BOOST_CHECK_EQUAL(*calls, 1);   // abort wins over the permissive policy
    // And the post-mortem survives the throw: the reader's own record of the
    // consumer backing out.
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Aborted);
    server.join();
}

// The reuse contract under complete(): one reader, cleared between attempts.
// Pins clear() itself — the end-state flags back to Streaming, the subclass
// accumulators wiped, the handler RUNNING again with a fresh channel — and
// the deliberate exception to the wipe: hooks survive, so one monitor
// observes every attempt's deltas.
BOOST_AUTO_TEST_CASE(clear_resets_the_reader_and_handler_for_reuse) {
    AcceptAllServer server(2);   // two full exchanges, one reader

    asio::io_context io;
    endpoint::ResolvedEndpoint where{
        .host = "127.0.0.1",
        .port = std::to_string(server.wait_listening()),
        .target = "/v1/complete",
        .tls = false};

    auto reader = std::make_shared<FakeReader>(
        std::make_shared<TerminalHandler>(io.get_executor()));
    const auto hook_fires = std::make_shared<int>(0);
    reader->add_hook(
        [hook_fires](const FakeDelta&) { ++*hook_fires; });

    // First exchange: completes, assembled, one delta observed.
    auto first = run_exchange(io, where, reader, OneEventDriver{});
    io.run();
    BOOST_CHECK_EQUAL(first.get().role, "assembled");
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Completed);
    BOOST_CHECK_EQUAL(*hook_fires, 1);

    // The reset: a closed channel cannot reopen, so the handler must have a
    // fresh one — get_state() RUNNING is observable, the channel itself is
    // proven by the second exchange below draining it.
    reader->clear();
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Streaming);
    BOOST_CHECK(!reader->finished());
    BOOST_CHECK(!reader->accumulated);
    BOOST_CHECK(!reader->assembled);
    BOOST_CHECK(reader->handler()->get_state() == FakeHandlerBase::State::RUNNING);

    // Second exchange over the SAME reader: a full terminal stream again,
    // and the surviving hook observed it too.
    io.restart();
    auto second = run_exchange(io, where, reader, OneEventDriver{});
    io.run();
    BOOST_CHECK_EQUAL(second.get().role, "assembled");
    BOOST_CHECK(reader->assembled);
    BOOST_CHECK(reader->end_state()
                == endpoint::ModelResponseReader<FakeDelta>::EndState::Completed);
    BOOST_CHECK_EQUAL(*hook_fires, 2);
    server.join();
}
