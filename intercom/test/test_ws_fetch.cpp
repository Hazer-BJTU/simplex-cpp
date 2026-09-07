// Deterministic, offline tests for the intercom WebSocket fetch API —
// connect_websocket (connect+upgrade+fold), fetch_once (one exchange:
// connect → send one message → read one reply → close → std::string), the
// fetch retry engine, and the recoverability verdict. Loopback WebSocket
// servers (plain ws:// and, for the wss flavour, a loopback TLS listener with
// a self-signed certificate) answer each session with a fixed exchange; a
// released loopback port supplies the refused-connect case. The fetch engine
// runs with millisecond backoffs so the retry loop stays fast and
// deterministic. Every test drives its io_context to quiescence, so nothing
// hangs on network timing.
#define BOOST_TEST_MODULE ws_fetch
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
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

// Self-signed "localhost" certificate (CN=localhost, SAN DNS:localhost +
// IP:127.0.0.1), valid 2026-09-06 .. 2036-09-03, for the offline wss://
// tests. The key is only ever used by the loopback test server.
static const char kTestCertPem[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDJTCCAg2gAwIBAgIUJO0V2q6/mno18riiE3YlE8KFCMAwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDkwNjEzNDkxN1oXDTM2MDkw
MzEzNDkxN1owFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAu3mtrnzLvSS7twB2AF4qI81QSw9/qUCX0DtI0XlvvQ/M
CQtYONLxYknElLH48Xr8JmkYeAW+oq8LxFkZRDjBm37sO2ZoTQXDatzQ4Qnt+cov
cASFTeYVtgVlCBe/iV/kGwVb2j9llWV/6QDDwXXXSi6sHV12YblP9dL3nzjTtUGI
e+qzL4pVwNjIYdUM0br7RHLB7RxlRlZqKBoIw339VV/9e6KJimQHJb+NjAfbxNbO
tTWH0JmFQRTS3yfWi6ewjp0vYYCuhAtF44pqYIStBZSYKh9Ug1ZkpzuAVeFptL29
FvLevlOyjfVlRTpZW9VYAoQrLOOTL6t0/xHKInMZ9QIDAQABo28wbTAdBgNVHQ4E
FgQUrBpwUslJ9ntchVwAhE7mYM3bsJUwHwYDVR0jBBgwFoAUrBpwUslJ9ntchVwA
hE7mYM3bsJUwDwYDVR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SH
BH8AAAEwDQYJKoZIhvcNAQELBQADggEBAHalcfXuveLphXDv63re/C4r6H+yi3Kd
ZITZvMhPRzEgGvKo4kRGSTZH9GvaMGl+ZZnJCoAvm8diiNfrYF7UFHEsYsd7Tn6f
UvWTkcjMCCKlG4K9JO+OhOnXITITT0FXMhN6XUsG2XY8cof1Z5K66Lxb8MMSaBGd
Cn6RyWgCkHgTe+GdVQMD0r8O8OzMcT1IjxF4iVpp9u22EBTAlhG0LY+zjfDqTmjO
Zhp5OW12XPfKo9gL7AAx8c/48fdAbuEpEWMRmmAuCdrGxirGhhKeKLPCG9YECM5j
al/aPhAJr2iR1zcvoXO7d0CDgBwmakk7eAWWo8Re8vmEEB+AHLM4zPY=
-----END CERTIFICATE-----
)PEM";

static const char kTestKeyPem[] = R"PEM(
-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC7ea2ufMu9JLu3
AHYAXiojzVBLD3+pQJfQO0jReW+9D8wJC1g40vFiScSUsfjxevwmaRh4Bb6irwvE
WRlEOMGbfuw7ZmhNBcNq3NDhCe35yi9wBIVN5hW2BWUIF7+JX+QbBVvaP2WVZX/p
AMPBdddKLqwdXXZhuU/10vefONO1QYh76rMvilXA2Mhh1QzRuvtEcsHtHGVGVmoo
GgjDff1VX/17oomKZAclv42MB9vE1s61NYfQmYVBFNLfJ9aLp7COnS9hgK6EC0Xj
impghK0FlJgqH1SDVmSnO4BV4Wm0vb0W8t6+U7KN9WVFOllb1VgChCss45Mvq3T/
Ecoicxn1AgMBAAECggEAGpeX2zg4bgvX1I1sL4Er2Qg2a283XHqdDhxap9vhzZ+A
AYhqayUAuEBecfkMprQbMBeYMO4frFPIB4Hb46FpVPUb7REJmmNG5NGNj14pM1VK
hUke333Tdo4tVoiH0qSXZn3MGZkEf7x1+EbzfW2JrCwSnde0AwiWHvhdx2f4H619
z0GPOLIh8n/BWOYfb6Z+9aj9ETWnRDSL288qloPsALgubXUD3Zcgs6vgAg9DPN+p
IlE08Yn6/J64W8vurV9P8LSZynYJ4bjYvUidHXXpV9qREcH+A2w4Ycw04FQsyicT
wqAjCyaqUSNsRXXe5PJknzMFbGZrB3DYXtSqeaasAQKBgQDilXnTeFprO5yilGeT
5lk6TIu8hfsLGlLXiZ5LIZxPAUrrJsk4ByRQwcCVimdhxlSy72kjb8actpGvB2No
f+7n140Jx5Aa9AVUDHoQtXGQRTzAHB3bdrCWzw1pONcqdeWs04oX6j4f6Ov1872y
44CD9Pq6zUZxjaIOoAimoinf8QKBgQDT0G1kD0YYN6OWXN0pAiF2KzxtG7WbjYGW
kyoMyZQQBKfSSUxIoOiETZ5fKxCwS/o7cVrz8dE91umq0s1k+icDUSemuozgdRH+
cwGGAOmBtvuCPC++hKeunXOGPrfuN7AGTW+ro8oTiASoy3rEkyx362YP5Muh6Yfc
6CZnZyOeRQKBgQCYXlpBSdLL/5dSgSex/poMKUNisFpkWfxRcvrenSiGvGDMBxYT
NkJGiDRgm2TwDDYS6goyyUyvP9px8C76K+XVRE9Uvz150pTus0E2kT1f/h9fNMkj
NwqDL5NeKdoPdJ7RfKOFd4D1ZmWezJzJelGG6yvciIQRgmPzH80ReUVdkQKBgAb8
zz8HyfYuj7T8J0edDGGLl5i520ngZzACdtapJ7tHjAnB5URYGpOSw/C7yPcn6n9f
g/KcPZzepCLAhYoZPoQ7fyVe7hrRgeB1Bs3W5d0jnjRzq9eLJMT76L26//JZ3/K1
R0PQSXBPgmfXHpuxhzwFhf5sO9OBkrvU5L9whZpJAoGAWnpQVYu+vQS4BZa2mNOh
TzHxCLpwLcHOnVUVpU2QNtOwBNYpCoPUZoM5ytvXedrJdKfmQnCiVgWRiPsmoiD2
yvlLWyyBtidTqM2rFzJ6qOyx6J/iaMwyQ8PQVHGuqppIlToTe+PKprnIG0xkmIau
3wmKwTXP6MHQqJFu7pyb5tw=
-----END PRIVATE KEY-----
)PEM";

endpoint::ResolvedEndpoint loopback_endpoint(unsigned short port,
                                             const char* target) {
    return endpoint::ResolvedEndpoint{
        .host = "127.0.0.1",
        .port = std::to_string(port),
        .target = target,
        .tls = false};
}

// A wss:// endpoint against localhost — the name the self-signed cert covers
// (CN/SAN), so hostname verification and SNI succeed.
endpoint::ResolvedEndpoint wss_endpoint(unsigned short port, const char* target) {
    return endpoint::ResolvedEndpoint{
        .host = "localhost",
        .port = std::to_string(port),
        .target = target,
        .tls = true};
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

// fetch_once(executor, endpoint, message, timeout, context) on a fresh
// io_context. The context defaults to the global one (used by the wss flavour;
// the plain flavour ignores it).
Outcome run_once(
    endpoint::ResolvedEndpoint endpoint,
    std::string message,
    std::size_t idle_timeout_sec = intercom::DEFAULT_WS_IDLE_TIMEOUT_SEC,
    endpoint::ssl_context& context = endpoint::get_global_ssl_context()) {
    asio::io_context io;
    Outcome outcome;
    asio::co_spawn(
        io,
        [&, message = std::move(message)]() -> asio::awaitable<void> {
            try {
                outcome.result = co_await intercom::fetch_once(
                    io.get_executor(), endpoint, message, idle_timeout_sec,
                    context);
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

// A fixed sequence of `n` dropped sessions — an ambiguous Read-stage transport
// failure each attempt, which an idempotent engine retries to its budget.
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

    auto outcome = run_once(loopback_endpoint(server.wait_listening(), "/v1/ws"), "ping");
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

    auto outcome = run_once(loopback_endpoint(server.wait_listening(), "/v1/ws"), "hi");
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_CHECK_EQUAL(*outcome.result, "hello world");
}

// The upgrade request's Host field is the endpoint's AUTHORITY — host plus a
// non-default port — so vhost routing and proxies see the same authority the
// HTTP transport sends. The loopback ephemeral port is non-default, so this
// pins the authority() path directly.
BOOST_AUTO_TEST_CASE(fetch_once_sends_the_authority_as_the_host_header) {
    std::string host_header;
    loopback_ws::OneShotServer server([&](tcp::socket& socket) {
        loopback_ws::serve_echo_record_host(socket, &host_header);
    });

    const unsigned short port = server.wait_listening();
    auto outcome = run_once(loopback_endpoint(port, "/v1/ws"), "ping");
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_CHECK_EQUAL(host_header, "127.0.0.1:" + std::to_string(port));
}

// A handshake the server rejects (401) is a connect failure, folded into
// WsException{Connect} carrying error::upgrade_declined.
BOOST_AUTO_TEST_CASE(fetch_once_folds_a_handshake_rejection) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        loopback_ws::serve_reject(socket, http::status::unauthorized, "no");
    });

    auto outcome = run_once(loopback_endpoint(server.wait_listening(), "/v1/ws"), "ping");
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
    auto outcome = run_once(loopback_endpoint(refused_port(), "/v1/ws"), "ping");

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

// A backend that never replies hits the idle deadline: WsTimeoutException, the
// WsException flavour dedicated to slow backends.
BOOST_AUTO_TEST_CASE(fetch_once_times_out_on_a_slow_backend) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        loopback_ws::serve_hold_after_read(socket, std::chrono::seconds(2));
    });

    auto outcome = run_once(
        loopback_endpoint(server.wait_listening(), "/v1/ws"), "ping", 1);
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

// --- fetch_once over wss:// ---------------------------------------------------

// The TLS flavour end to end: TCP + TLS handshake + SNI + hostname
// verification (against a custom context that trusts the self-signed cert) +
// WebSocket upgrade + one message round trip + graceful close. Success proves
// the wss flavour was selected and the certificate verified.
BOOST_AUTO_TEST_CASE(fetch_once_round_trips_over_wss) {
    loopback_ws::TlsEchoServer server(kTestCertPem, kTestKeyPem);

    asio::ssl::context client_ctx(asio::ssl::context::tls_client);
    client_ctx.add_certificate_authority(asio::buffer(kTestCertPem));
    client_ctx.set_verify_mode(asio::ssl::verify_peer);

    auto outcome = run_once(
        wss_endpoint(server.wait_listening(), "/v1/ws"), "ping",
        intercom::DEFAULT_WS_IDLE_TIMEOUT_SEC, client_ctx);
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_CHECK_EQUAL(*outcome.result, "ping");
}

// The default (system trust store) context does NOT trust the self-signed
// cert: certificate verification fails closed, surfacing as WsException{Connect}.
BOOST_AUTO_TEST_CASE(fetch_once_rejects_an_untrusted_certificate_over_wss) {
    loopback_ws::TlsEchoServer server(kTestCertPem, kTestKeyPem);

    auto outcome = run_once(
        wss_endpoint(server.wait_listening(), "/v1/ws"), "ping");
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const intercom::WsException& e) {
        BOOST_CHECK(e.stage() == intercom::WsException::Stage::Connect);
    } catch (...) {
        BOOST_FAIL("expected WsException for the untrusted certificate");
    }
}

// --- fetch: the retry engine ---------------------------------------------------

// An idempotent engine retries an ambiguous Read-stage drop and the second
// session succeeds with the SAME message re-sent.
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
        std::chrono::milliseconds(2), 2, /*idempotent=*/true);
    auto outcome = run_fetch(io, server.wait_listening(), engine, "ping");
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_CHECK_EQUAL(*outcome.result, "recovered");
}

// A NON-idempotent engine must NOT re-send after an ambiguous Read failure:
// the server may already have acted on the request. The failure propagates
// after one attempt.
BOOST_AUTO_TEST_CASE(fetch_does_not_retry_a_read_failure_when_not_idempotent) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        loopback_ws::serve_drop_after_read(socket);
    });

    asio::io_context io;
    intercom::fetch engine(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 3);   // idempotent = false (default)
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

// A handshake rejection (upgrade_declined) is NOT recoverable: the same
// request draws the same answer, so the engine must not retry — zero sleeps.
BOOST_AUTO_TEST_CASE(fetch_does_not_retry_a_handshake_rejection) {
    loopback_ws::OneShotServer server([](tcp::socket& socket) {
        loopback_ws::serve_reject(socket, http::status::unauthorized, "no");
    });

    asio::io_context io;
    RecordingFetch engine(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 3, /*idempotent=*/true);
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
        std::chrono::milliseconds(2), 0, /*idempotent=*/true);
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
        std::chrono::milliseconds(100), 3, /*idempotent=*/true);
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
        std::chrono::milliseconds(100), 1, /*idempotent=*/true);

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

// Pins is_recoverable directly: the module's own table, keyed on WsException
// plus the idempotency flag.
BOOST_AUTO_TEST_CASE(is_recoverable_verdict_table) {
    using Stage = intercom::WsException::Stage;
    const auto failure = [](Stage stage, boost::system::error_code ec = {}) {
        return intercom::WsException(stage, "msg", ec, "127.0.0.1", "/v1/ws");
    };

    // Connect: transient transport errors are recoverable (idempotency is
    // irrelevant — the request never left the client).
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::error::connection_refused), false));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::error::connection_reset), false));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::error::host_not_found_try_again), false));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::error::timed_out), false));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Connect, asio::ssl::error::stream_truncated), false));
    BOOST_CHECK(!intercom::is_recoverable(
        failure(Stage::Connect, asio::error::host_not_found), false));
    BOOST_CHECK(!intercom::is_recoverable(
        failure(Stage::Connect, websocket::error::upgrade_declined), false));

    // Write/Read: ambiguous — recoverable only when idempotent AND the error
    // is an unambiguous transient transport failure.
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Read, asio::error::connection_reset), true));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Read, asio::error::eof), true));
    BOOST_CHECK(intercom::is_recoverable(
        failure(Stage::Write, boost::beast::error::timeout), true));
    // Not idempotent -> never retry an ambiguous failure.
    BOOST_CHECK(!intercom::is_recoverable(
        failure(Stage::Read, asio::error::connection_reset), false));
    // Cancellation and protocol/close errors are never retried, even when
    // idempotent.
    BOOST_CHECK(!intercom::is_recoverable(
        failure(Stage::Read, asio::error::operation_aborted), true));
    BOOST_CHECK(!intercom::is_recoverable(
        failure(Stage::Read, websocket::error::closed), true));
    BOOST_CHECK(!intercom::is_recoverable(
        failure(Stage::Read, websocket::error::message_too_big), true));

    // Unknown never.
    BOOST_CHECK(!intercom::is_recoverable(failure(Stage::Unknown), true));
}

