// Deterministic, offline tests for the bounded single-shot request API —
// endpoint::connect (the connect+fold primitive), fetch_once (one bounded
// exchange: connect → write → read the whole response → status-check →
// handler), and the fetch retry engine. A loopback server answers each
// exchange with a fixed status/body; a released loopback port supplies the
// refused-connect case. The fetch engine runs with millisecond backoffs so
// the retry loop stays fast and deterministic. Every test drives its
// io_context to quiescence, so nothing hangs on network timing.
#define BOOST_TEST_MODULE fetch
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "endpoint/fetch.hpp"
#include "endpoint/http_request_exception.hpp"

#include "loopback_server.hpp"

namespace asio = boost::asio;
namespace http = boost::beast::http;

namespace {

using Request = endpoint::ModelRequestInterpreter::HttpRequest;

// A GET request against the loopback host, shaped as the provider-info
// queries build theirs (Host + Accept: application/json).
Request build_get_request(const char* target) {
    Request request{http::verb::get, target, 11};
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::accept, "application/json");
    return request;
}

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

// --- fetch_once harnesses -----------------------------------------------------

// JSON convenience overload: fetch_once(executor, endpoint, request, timeout).
struct JsonOutcome {
    std::optional<nlohmann::json> result;
    std::exception_ptr failure;
};

JsonOutcome run_once_json(
    unsigned short port,
    std::size_t read_timeout_sec = endpoint::DEFAULT_HTTP_READ_TIMEOUT_SEC) {
    asio::io_context io;
    JsonOutcome outcome;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            outcome.result = co_await endpoint::fetch_once(
                io.get_executor(), loopback_endpoint(port, "/v1/models"),
                build_get_request("/v1/models"), read_timeout_sec);
        } catch (...) {
            outcome.failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    return outcome;
}

// Handler form: fetch_once(executor, endpoint, request, handler, timeout).
// The handler is invoked as an lvalue, so the product type is keyed on
// Handler& (consistent with fetch.hpp's generic signature).
template<typename Handler>
struct HandlerOutcome {
    std::optional<std::invoke_result_t<Handler&, std::string>> result;
    std::exception_ptr failure;
};

template<typename Handler>
HandlerOutcome<Handler> run_once(
    unsigned short port, Handler handler,
    std::size_t read_timeout_sec = endpoint::DEFAULT_HTTP_READ_TIMEOUT_SEC) {
    asio::io_context io;
    HandlerOutcome<Handler> outcome;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            outcome.result = co_await endpoint::fetch_once(
                io.get_executor(), loopback_endpoint(port, "/v1/models"),
                build_get_request("/v1/models"), handler, read_timeout_sec);
        } catch (...) {
            outcome.failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    return outcome;
}

// --- fetch (retry engine) harnesses -------------------------------------------

// JSON convenience operator(): engine(endpoint, request). The engine is bound
// to @p io, so the exchange runs and drains on that SAME io — the caller
// constructs the engine on it and this runner runs it to quiescence.
JsonOutcome run_fetch_json(
    asio::io_context& io, unsigned short port, endpoint::fetch& engine) {
    JsonOutcome outcome;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            outcome.result = co_await engine(
                loopback_endpoint(port, "/v1/models"),
                build_get_request("/v1/models"));
        } catch (...) {
            outcome.failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    return outcome;
}

// Handler form: engine(endpoint, request, handler), on the same @p io the
// engine is bound to.
template<typename Handler>
HandlerOutcome<Handler> run_fetch(
    asio::io_context& io, unsigned short port, endpoint::fetch& engine,
    Handler handler) {
    HandlerOutcome<Handler> outcome;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            outcome.result = co_await engine(
                loopback_endpoint(port, "/v1/models"),
                build_get_request("/v1/models"), handler);
        } catch (...) {
            outcome.failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    return outcome;
}

// Records the backoff value each _sleep() waits (before delegating to the
// base, which performs the actual wait and the advance), so a test can pin
// the retry timing without measuring wall-clock time.
struct RecordingFetch : endpoint::fetch {
    using endpoint::fetch::fetch;
    std::vector<std::chrono::milliseconds> waits;
    boost::asio::awaitable<void> _sleep() override {
        waits.push_back(_backoff);
        co_await endpoint::retry_policy::_sleep();
    }
};

// A fixed sequence of `n` 503 answers — a recoverable provider overload each
// attempt, so the engine retries to its budget.
std::vector<loopback::Serve> serve_503s(std::size_t n) {
    std::vector<loopback::Serve> sequence;
    for (std::size_t i = 0; i < n; ++i) {
        sequence.push_back([](asio::ip::tcp::socket& socket) {
            loopback::serve_fixed_response(
                socket, http::status::service_unavailable,
                R"({"error": "overloaded"})");
        });
    }
    return sequence;
}

// Exposes the shared retry_policy's protected verdict for the table test.
struct PolicyProbe : endpoint::retry_policy {
    using endpoint::retry_policy::retry_policy;
    bool recoverable(const HttpRequestException& failure) noexcept {
        return _recoverable(failure);
    }
};

} // namespace

// --- endpoint::connect --------------------------------------------------------

// connect() folds a refused connection into HttpRequestException{Connect}
// with the transport error code and the endpoint's target/host context —
// the primitive both engines share, pinned directly here.
BOOST_AUTO_TEST_CASE(connect_folds_a_refused_connection_into_stage_connect) {
    asio::io_context io;
    const auto where = loopback_endpoint(refused_port(), "/v1/models");

    auto operation = endpoint::connect(io.get_executor(), where);
    auto result = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();

    try {
        result.get();
        BOOST_FAIL("connect() did not propagate the refused connection");
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::Connect);
        BOOST_CHECK_EQUAL(e.host(), "127.0.0.1");
        BOOST_CHECK_EQUAL(e.target(), "/v1/models");
        // THE point of the fold: the system_error's code survives.
        BOOST_CHECK(e.error_code() == asio::error::connection_refused);
    }
}

// --- fetch_once: the bounded single-shot --------------------------------------

// The happy path over the JSON convenience overload: a 200 body parses to
// json with no handler named at the call site.
BOOST_AUTO_TEST_CASE(fetch_once_parses_the_200_body_as_json) {
    const std::string catalogue = nlohmann::json{
        {"id", "fixture-mini"},
        {"object", "model"},
    }.dump();
    loopback::OneShotServer server([&](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, catalogue);
    });

    auto outcome = run_once_json(server.wait_listening());
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_CHECK(outcome.result->is_object());
    BOOST_CHECK_EQUAL((*outcome.result)["id"], "fixture-mini");
}

// A non-200 reply is a failure: the status code and the (bounded) body are
// folded into the HandleResponse failure so the provider's diagnosis survives.
BOOST_AUTO_TEST_CASE(fetch_once_folds_a_non_200_body_into_the_failure) {
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::unauthorized,
                                       R"({"error": "bad key"})");
    });

    auto outcome = run_once_json(server.wait_listening());
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::HandleResponse);
        BOOST_CHECK_EQUAL(e.status(), 401u);
        BOOST_CHECK(e.method() == "GET");
        BOOST_CHECK_EQUAL(e.target(), "/v1/models");
        BOOST_CHECK_NE(std::string(e.what()).find("bad key"),
                       std::string::npos);
    } catch (...) {
        BOOST_FAIL("expected an HttpRequestException for the 401 body");
    }
}

// A 200 body that is not JSON is a decode fault: the stock json_handler
// raises HandleResponse (no status) and the request context rides along.
BOOST_AUTO_TEST_CASE(fetch_once_rejects_a_non_json_body) {
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, "not json");
    });

    auto outcome = run_once_json(server.wait_listening());
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::HandleResponse);
        BOOST_CHECK_EQUAL(e.status(), 0u);
        BOOST_CHECK_EQUAL(e.target(), "/v1/models");
        BOOST_CHECK_NE(std::string(e.what()).find("not JSON"),
                       std::string::npos);
    } catch (...) {
        BOOST_FAIL("expected an HttpRequestException for the non-JSON body");
    }
}

// The handler is the abstraction point: a custom callable turns the 200 body
// into any product — here the raw body string passes through, proving
// fetch_once is not JSON-only.
BOOST_AUTO_TEST_CASE(fetch_once_runs_a_custom_handler) {
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, "payload");
    });

    auto outcome = run_once(
        server.wait_listening(),
        [](std::string body) { return body; });
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_CHECK_EQUAL(*outcome.result, "payload");
}

// A handler that throws a stray std::exception is wrapped with the request
// context (the caller's own bug surfaces as HandleResponse, still classifiable).
BOOST_AUTO_TEST_CASE(fetch_once_wraps_a_handler_fault_with_request_context) {
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, "{}");
    });

    auto outcome = run_once(
        server.wait_listening(),
        [](std::string) -> std::string { throw std::runtime_error("boom"); });
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::HandleResponse);
        BOOST_CHECK_EQUAL(e.method(), "GET");
        BOOST_CHECK_EQUAL(e.target(), "/v1/models");
        BOOST_CHECK_NE(std::string(e.what()).find("boom"), std::string::npos);
    } catch (...) {
        BOOST_FAIL("expected the handler fault wrapped as HttpRequestException");
    }
}

// A handler that already raised the module's lifecycle exception keeps it
// unchanged — fetch_once never double-wraps a HttpRequestException.
BOOST_AUTO_TEST_CASE(fetch_once_passes_a_handler_http_exception_through) {
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, "{}");
    });

    auto outcome = run_once(
        server.wait_listening(),
        [](std::string) -> std::string {
            throw HttpRequestException(
                HttpRequestException::Stage::HandleResponse,
                "custom verdict", {}, "GET", "/x", "h", 418);
        });
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK_EQUAL(e.status(), 418u);
        BOOST_CHECK_EQUAL(e.target(), "/x");
    } catch (...) {
        BOOST_FAIL("expected the handler's own HttpRequestException");
    }
}

// fetch_once connects through the shared primitive, so a refused connection
// surfaces as Stage::Connect with the transport code preserved.
BOOST_AUTO_TEST_CASE(fetch_once_wraps_a_connect_failure) {
    auto outcome = run_once_json(refused_port());

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::Connect);
        BOOST_CHECK(e.error_code() == asio::error::connection_refused);
    } catch (...) {
        BOOST_FAIL("expected HttpRequestException for the refused connect");
    }
}

// --- fetch: the bounded retry engine ------------------------------------------

// A recoverable 5xx on the first exchange backs off and retries; the second
// exchange succeeds — the SAME request re-sent, the SAME handler re-invoked.
BOOST_AUTO_TEST_CASE(fetch_retries_a_transient_5xx_and_succeeds) {
    loopback::SequenceServer server({
        [](asio::ip::tcp::socket& socket) {
            loopback::serve_fixed_response(
                socket, http::status::service_unavailable,
                R"({"error": "overloaded"})");
        },
        [](asio::ip::tcp::socket& socket) {
            loopback::serve_fixed_response(
                socket, http::status::ok,
                nlohmann::json{{"id", "recovered"}}.dump());
        },
    });

    asio::io_context io;
    endpoint::fetch engine(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 2);
    auto outcome = run_fetch_json(io, server.wait_listening(), engine);
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_CHECK_EQUAL((*outcome.result)["id"], "recovered");
}

// A provider rejection the same request would draw again (401) is not
// retried: the original failure propagates immediately after one attempt.
BOOST_AUTO_TEST_CASE(fetch_does_not_retry_a_non_recoverable_rejection) {
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::unauthorized,
                                       R"({"error": "bad key"})");
    });

    asio::io_context io;
    endpoint::fetch engine(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 2);
    auto outcome = run_fetch_json(io, server.wait_listening(), engine);
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK_EQUAL(e.status(), 401u);
    } catch (...) {
        BOOST_FAIL("expected HttpRequestException for the 401 rejection");
    }
}

// A budget of 0 retries is legitimate: exactly one attempt, and even a
// classification that says recoverable cannot conjure a retry out of an
// empty budget.
BOOST_AUTO_TEST_CASE(fetch_with_zero_retries_runs_a_single_attempt) {
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(
            socket, http::status::service_unavailable, R"({"error": "overloaded"})");
    });

    asio::io_context io;
    endpoint::fetch engine(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 0);
    auto outcome = run_fetch_json(io, server.wait_listening(), engine);
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK_EQUAL(e.status(), 503u);
    } catch (...) {
        BOOST_FAIL("expected HttpRequestException for the 503 with no retry");
    }
}

// A handler fault (fetch_once wraps the handler's throw as a status-less
// HandleResponse) is a LOCAL decoder bug, not a provider answer: fetch must
// NOT retry it, however much budget remains — the same request draws the
// same fault.
BOOST_AUTO_TEST_CASE(fetch_does_not_retry_handler_failure) {
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, "{}");
    });

    asio::io_context io;
    endpoint::fetch engine(
        io.get_executor(), std::chrono::milliseconds(1),
        std::chrono::milliseconds(2), 3);
    const auto calls = std::make_shared<int>(0);
    auto outcome = run_fetch(
        io, server.wait_listening(), engine,
        [calls](std::string) -> std::string {
            ++*calls;
            throw std::logic_error("decoder bug");
        });
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::HandleResponse);
        BOOST_CHECK_EQUAL(e.status(), 0u);
    } catch (...) {
        BOOST_FAIL("expected HttpRequestException for the handler fault");
    }
    BOOST_CHECK_EQUAL(*calls, 1);   // surfaced immediately, no retry
}

// The backoff sequence: the first retry waits exactly initial_backoff, and
// each subsequent retry doubles — initial, 2×initial, 4×initial, ….
BOOST_AUTO_TEST_CASE(fetch_backoff_advances_initial_then_doubles) {
    loopback::SequenceServer server(serve_503s(4));   // 4 attempts, 3 sleeps

    asio::io_context io;
    RecordingFetch engine(
        io.get_executor(), std::chrono::milliseconds(10),
        std::chrono::milliseconds(100), 3);
    auto outcome = run_fetch_json(io, server.wait_listening(), engine);
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
        loopback::SequenceServer server(serve_503s(2));
        auto outcome = run_fetch_json(io, server.wait_listening(), engine);
        server.join();
        BOOST_REQUIRE(outcome.failure);
        BOOST_REQUIRE_EQUAL(engine.waits.size(), 1u);
        BOOST_CHECK_EQUAL(engine.waits[0], std::chrono::milliseconds(10));
    }

    {
        io.restart();
        loopback::SequenceServer server(serve_503s(2));
        auto outcome = run_fetch_json(io, server.wait_listening(), engine);
        server.join();
        BOOST_REQUIRE(outcome.failure);
        BOOST_REQUIRE_EQUAL(engine.waits.size(), 2u);
        // Started over at initial_backoff — not the 20ms the first call left.
        BOOST_CHECK_EQUAL(engine.waits[1], std::chrono::milliseconds(10));
    }
}

// --- the shared retry policy --------------------------------------------------

// Pins retry_policy's default verdict directly: the base class both engines
// inherit, independent of either engine's wiring.
BOOST_AUTO_TEST_CASE(retry_policy_default_verdict_table) {
    asio::io_context io;
    PolicyProbe probe{io.get_executor()};

    using Stage = HttpRequestException::Stage;
    const auto failure = [](Stage stage, unsigned status = 0,
                            boost::system::error_code ec = {}) {
        return HttpRequestException(stage, "msg", ec, "GET", "/t", "h", status);
    };

    // Status-led.
    BOOST_CHECK(probe.recoverable(failure(Stage::HandleResponse, 429)));
    BOOST_CHECK(probe.recoverable(failure(Stage::HandleResponse, 503)));
    BOOST_CHECK(!probe.recoverable(failure(Stage::HandleResponse, 401)));
    BOOST_CHECK(!probe.recoverable(failure(Stage::HandleResponse, 400)));
    BOOST_CHECK(!probe.recoverable(failure(Stage::HandleResponse, 404)));

    // Stage-led (no status).
    BOOST_CHECK(probe.recoverable(failure(Stage::Write)));
    BOOST_CHECK(probe.recoverable(failure(Stage::HandleResponse)));
    BOOST_CHECK(!probe.recoverable(failure(Stage::CreateRequest)));
    BOOST_CHECK(!probe.recoverable(failure(Stage::Unknown)));

    // Connect error-code categories.
    BOOST_CHECK(probe.recoverable(
        failure(Stage::Connect, 0, asio::error::connection_refused)));
    BOOST_CHECK(probe.recoverable(
        failure(Stage::Connect, 0, asio::error::host_not_found_try_again)));
    BOOST_CHECK(!probe.recoverable(
        failure(Stage::Connect, 0, asio::error::host_not_found)));
    BOOST_CHECK(probe.recoverable(
        failure(Stage::Connect, 0, asio::ssl::error::stream_truncated)));
}
