// Deterministic, offline tests for the intercom WebSocket fetch API —
// connect_websocket (connect+upgrade+fold), fetch_once (one bounded exchange:
// connect → send one message → read one reply → close → std::string), the
// fetch retry engine, and the is_recoverable verdict. A loopback WebSocket
// server answers each session with a fixed exchange; a released loopback port
// supplies the refused-connect case. The fetch engine runs with millisecond
// backoffs so the retry loop stays fast and deterministic. Every test drives
// its io_context to quiescence, so nothing hangs on network timing.
#define BOOST_TEST_MODULE ws_fetch
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/websocket.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "intercom/fetch.hpp"
#include "intercom/ws_exception.hpp"

#include "loopback_ws_server.hpp"

namespace {

endpoint::ResolvedEndpoint loopback_endpoint(unsigned short port,
                                             const char* target) {
    return endpoint::ResolvedEndpoint{
        .host = "127.0.0.1",
        .port = std::to_string(port),
        .target = target,
        .tls = false};
}

// A loopback port that nobody listens on: bind, read the port, release.
unsigned short refused_port() {
    asio::io_context io;
    asio::ip::tcp::acceptor probe(
        io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const auto port = probe.local_endpoint().port();
    probe.close();
    return port;
}

// --- harnesses ----------------------------------------------------------------

struct Outcome {
    std::optional<std::string> result;
    std::exception_ptr failure;
};

// fetch_once(executor, endpoint, message, timeout) on a fresh io_context.
Outcome run_once(
    unsigned short port,
    std::string message,
    std::size_t read_timeout_sec = intercom::DEFAULT_WS_READ_TIMEOUT_SEC) {
    asio::io_context io;
    Outcome outcome;
    asio::co_spawn(
        io,
        [&, message = std::move(message)]() -> asio::awaitable<void> {
            try {
                outcome.result = co_await intercom::fetch_once(
                    io.get_executor(), loopback_endpoint(port, "/v1/ws"),
                    message, read_timeout_sec);
            } catch (...) {
                outcome.failure = std::current_exception();
            }
        },
        asio::detached);
    io.run();
    return outcome;
}

// engine(endpoint, message) on the SAME io the engine is bound to — the
// caller constructs the engine on it and this runner runs it to quiescence.
Outcome run_fetch(
    asio::io_context& io,
    unsigned short port,
    intercom::fetch& engine,
    std::string message) {
    Outcome outcome;
    asio::co_spawn(
        io,
        [&, message = std::move(message)]() -> asio::awaitable<void> {
            try {
                outcome.result = co_await engine(
                    loopback_endpoint(port, "/v1/ws"), message);
            } catch (...) {
                outcome.failure = std::current_exception();
            }
        },
        asio::detached);
    io.run();
    return outcome;
}

// Records the backoff value each _sleep() waits (before delegating to the
// base, which performs the actual wait and the advance), so a test can pin
// the retry timing without measuring wall-clock time.
struct RecordingFetch : intercom::fetch {
    using intercom::fetch::fetch;
    std::vector<std::chrono::milliseconds> waits;
    boost::asio::awaitable<void> _sleep() override {
        waits.push_back(_backoff);
        co_await endpoint::retry_policy::_sleep();
    }
};

// A fixed sequence of `n` dropped sessions — a recoverable transport failure
// each attempt, so the engine retries to its budget.
std::vector<loopback_ws::Serve> serve_drops(std::size_t n) {
    std::vector<loopback_ws::Serve> sequence;
    for (std::size_t i = 0; i < n; ++i) {
        sequence.push_back([](tcp::socket& socket) {
            loopback_ws::serve_drop_after_read(socket);
        });
    }
    return sequence;
}

} // namespace

// --- connect_websocket --------------------------------------------------------

// connect_websocket folds a refused connection into WsException{Connect} with
// the transport error code and the endpoint's host/target context.
BOOST_AUTO_TEST_CASE(connect_websocket_folds_a_refused_connection) {
    asio::io_context io;
    const auto where = loopback_endpoint(refused_port(), "/v1/ws");

    auto operation = intercom::connect_websocket(io.get_executor(), where);
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();

    try {
        result.get();
        BOOST_FAIL("connect_websocket() did not propagate the refused connection");
    } catch (const intercom::WsException& e) {
        BOOST_CHECK(e.stage() == intercom::WsException::Stage::Connect);
        BOOST_CHECK_EQUAL(e.host(), "127.0.0.1");
        BOOST_CHECK_EQUAL(e.target(), "/v1/ws");
        BOOST_CHECK(e.error_code() == asio::error::connection_refused);
    }
}

// --- fetch_once ---------------------------------------------------------------

// The happy path: one text message out, the echo comes back as a std::string,
// and the server confirms the client sent a TEXT frame (the module contract).
BOOST_AUTO_TEST_CASE(fetch_once_round_trips_a_text_message) {
    bool got_text = false;
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        loopback_ws::serve_echo(socket, &got_text);
    });

    auto outcome = run_once(server.wait_listening(), "ping");
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_CHECK_EQUAL(*outcome.result, "ping");
    BOOST_CHECK(got_text);   // the payload was sent as a text frame
}

// A reply split across fragments is reassembled into ONE std::string before
// the caller sees it — the message-oriented read, not a raw frame read.
BOOST_AUTO_TEST_CASE(fetch_once_reassembles_a_fragmented_reply) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        loopback_ws::serve_fragmented(socket, "hello", " world");
    });

    auto outcome = run_once(server.wait_listening(), "hi");
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_CHECK_EQUAL(*outcome.result, "hello world");
}

// A handshake the server rejects (401) is a connect failure, folded into
// WsException{Connect} carrying error::upgrade_declined.
BOOST_AUTO_TEST_CASE(fetch_once_folds_a_handshake_rejection) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        loopback_ws::serve_reject(socket, http::status::unauthorized, "no");
    });

    auto outcome = run_once(server.wait_listening(), "ping");
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const intercom::WsException& e) {
        BOOST_CHECK(e.stage() == intercom::WsException::Stage::Connect);
        BOOST_CHECK(e.error_code() == websocket::error::upgrade_declined);
    } catch (...) {
        BOOST_FAIL("expected WsException for the rejected handshake");
    }
}

// fetch_once connects through the shared primitive, so a refused connection
// surfaces as Stage::Connect with the transport code preserved.
BOOST_AUTO_TEST_CASE(fetch_once_wraps_a_connect_failure) {
    auto outcome = run_once(refused_port(), "ping");

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const intercom::WsException& e) {
        BOOST_CHECK(e.stage() == intercom::WsException::Stage::Connect);
        BOOST_CHECK(e.error_code() == asio::error::connection_refused);
    } catch (...) {
        BOOST_FAIL("expected WsException for the refused connect");
    }
}

// A backend that never replies hits the read deadline: WsTimeoutException, the
// WsException flavour dedicated to slow backends.
BOOST_AUTO_TEST_CASE(fetch_once_times_out_on_a_slow_backend) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        loopback_ws::serve_hold_after_read(socket, std::chrono::seconds(2));
    });

    auto outcome = run_once(server.wait_listening(), "ping", 1);
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const intercom::WsTimeoutException& e) {
        BOOST_CHECK(e.stage() == intercom::WsException::Stage::Read);
    } catch (...) {
        BOOST_FAIL("expected WsTimeoutException for the slow backend");
    }
}

// --- fetch: the retry engine ---------------------------------------------------

// A transient mid-exchange drop (recoverable, stage Read) backs off and
// retries; the second session succeeds with the SAME message re-sent.
BOOST_AUTO_TEST_CASE(fetch_retries_a_transient_failure_and_succeeds) {
    loopback_ws::SequenceServer server({
        [](tcp::socket& socket) {
            loopback_ws::serve_drop_after_read(socket);
        },
        [](tcp::socket& socket) {
            loopback_ws::serve_reply(socket, "recovered");
        },
    });

    asio::io_context io;
    intercom::fetch engine(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 2);
    auto outcome = run_fetch(io, server.wait_listening(), engine, "ping");
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_CHECK_EQUAL(*outcome.result, "recovered");
}

// A handshake rejection (upgrade_declined) is NOT recoverable: the same
// request draws the same answer, so the engine must not retry — zero sleeps.
BOOST_AUTO_TEST_CASE(fetch_does_not_retry_a_handshake_rejection) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        loopback_ws::serve_reject(socket, http::status::unauthorized, "no");
    });

    asio::io_context io;
    RecordingFetch engine(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 3);
    auto outcome = run_fetch(io, server.wait_listening(), engine, "ping");
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const intercom::WsException& e) {
        BOOST_CHECK(e.error_code() == websocket::error::upgrade_declined);
    } catch (...) {
        BOOST_FAIL("expected WsException for the rejected handshake");
    }
    BOOST_CHECK(engine.waits.empty());   // surfaced immediately, no retry
}

// A budget of 0 retries is legitimate: exactly one attempt, and even a
// classification that says recoverable cannot conjure a retry out of an
// empty budget.
BOOST_AUTO_TEST_CASE(fetch_with_zero_retries_runs_a_single_attempt) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        loopback_ws::serve_drop_after_read(socket);
    });

    asio::io_context io;
    intercom::fetch engine(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 0);
    auto outcome = run_fetch(io, server.wait_listening(), engine, "ping");
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const intercom::WsException& e) {
        BOOST_CHECK(e.stage() == intercom::WsException::Stage::Read);
    } catch (...) {
        BOOST_FAIL("expected WsException for the dropped session");
    }
}

// The backoff sequence: the first retry waits exactly initial_backoff, and
// each subsequent retry doubles — initial, 2×initial, 4×initial, ….
BOOST_AUTO_TEST_CASE(fetch_backoff_advances_initial_then_doubles) {
    loopback_ws::SequenceServer server(serve_drops(4));   // 4 attempts, 3 sleeps

    asio::io_context io;
    RecordingFetch engine(
        io.get_executor(), std::chrono::milliseconds(10),
        std::chrono::milliseconds(100), 3);
    auto outcome = run_fetch(io, server.wait_listening(), engine, "ping");
    server.join();

    BOOST_REQUIRE(outcome.failure);   // the budget is exhausted
    BOOST_REQUIRE_EQUAL(engine.waits.size(), 3u);
    BOOST_CHECK_EQUAL(engine.waits[0], std::chrono::milliseconds(10));
    BOOST_CHECK_EQUAL(engine.waits[1], std::chrono::milliseconds(20));
    BOOST_CHECK_EQUAL(engine.waits[2], std::chrono::milliseconds(40));
}

// Two sequential calls on the SAME engine both start from initial_backoff:
// the per-call reset prevents one call's exhausted backoff from leaking into
// the next.
BOOST_AUTO_TEST_CASE(fetch_resets_backoff_between_calls) {
    asio::io_context io;
    RecordingFetch engine(
        io.get_executor(), std::chrono::milliseconds(10),
        std::chrono::milliseconds(100), 1);   // 2 attempts, 1 sleep per call

    {
        loopback_ws::SequenceServer server(serve_drops(2));
        auto outcome = run_fetch(io, server.wait_listening(), engine, "ping");
        server.join();
        BOOST_REQUIRE(outcome.failure);
        BOOST_REQUIRE_EQUAL(engine.waits.size(), 1u);
        BOOST_CHECK_EQUAL(engine.waits[0], std::chrono::milliseconds(10));
    }

    {
        io.restart();
        loopback_ws::SequenceServer server(serve_drops(2));
        auto outcome = run_fetch(io, server.wait_listening(), engine, "ping");
        server.join();
        BOOST_REQUIRE(outcome.failure);
        BOOST_REQUIRE_EQUAL(engine.waits.size(), 2u);
        // Started over at initial_backoff — not the 20ms the first call left.
        BOOST_CHECK_EQUAL(engine.waits[1], std::chrono::milliseconds(10));
    }
}

// --- the recoverability verdict ------------------------------------------------

// Pins is_recoverable directly: the module's own table, keyed on WsException.
BOOST_AUTO_TEST_CASE(is_recoverable_verdict_table) {
    using Stage = intercom::WsException::Stage;
    const auto failure = [](Stage stage, boost::system::error_code ec = {}) {
        return intercom::WsException(stage, "msg", ec, "127.0.0.1", "/v1/ws");
    };

    // Connect error-code categories.
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::error::connection_refused)));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::error::connection_reset)));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::error::host_not_found_try_again)));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::error::timed_out)));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::ssl::error::stream_truncated)));
    BOOST_CHECK(!intercom::is_recoverable(
        failure(Stage::Connect, asio::error::host_not_found)));
    BOOST_CHECK(!intercom::is_recoverable(
        failure(Stage::Connect, websocket::error::upgrade_declined)));

    // Stage-led (no error code).
    BOOST_CHECK(intercom::is_recoverable(failure(Stage::Write)));
    BOOST_CHECK(intercom::is_recoverable(failure(Stage::Read)));
    BOOST_CHECK(!intercom::is_recoverable(failure(Stage::Unknown)));
}

