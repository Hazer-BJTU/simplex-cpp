#pragma once

//
// fetch.hpp — the bounded single-shot request API: fetch_once + the fetch
// retry engine
// ====================================================================
//
// The bounded counterpart of complete/complete_once: one complete HTTP
// request/response exchange whose WHOLE body is read before the caller sees
// anything, as opposed to the streaming reader/consumer pair complete_once
// wires up. It exists for queries that are cheap, whole-body, and not SSE —
// the provider's model catalogue, an account balance, any JSON endpoint.
//
// Layering mirrors the streaming side, one level lower each time:
//
//   connect(executor, resolved)   — connection + Connect-stage folding
//                                    (shared with complete_once)
//   fetch_once(...)               — ONE bounded exchange: connect, write,
//                                    read the whole response, status-check
//                                    (a non-200 body folds into the failure),
//                                    then hand the 200 body to a handler
//                                    callable `Product(std::string)`.
//   fetch                         — fetch_once plus the retry policy shared
//                                    with complete (retry_policy): on a
//                                    recoverable failure the SAME request is
//                                    re-sent verbatim and the SAME handler is
//                                    re-invoked, up to the retry budget.
//
// The default handler is json_handler — parse the body as nlohmann::json —
// and the default return type is therefore nlohmann::json: the JSON
// convenience overloads below make `co_await fetch_once(executor, resolved,
// request)` return json with no handler named at the call site.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include "endpoint/model_request.hpp"
#include "endpoint/request.hpp"
#include "endpoint/retry_policy.hpp"

#include "logging/logger.hpp"

namespace endpoint {

/**
 * @brief The stock bounded-response handler: parse the body as JSON.
 *
 * 200 response body in, nlohmann::json out. A body that is not JSON raises a
 * plain std::runtime_error — deliberately NOT HttpRequestException, so
 * fetch_once's handler-fault path wraps it with the request context
 * (method/target/host) as HttpRequestException{Stage::HandleResponse}; a
 * decode failure keeps its request line for the logs, exactly as the
 * non-200 path does.
 *
 * This is the handler behind fetch_once/fetch's JSON convenience overloads:
 * a call that names no handler gets this one, so the default return type is
 * nlohmann::json.
 */
inline nlohmann::json json_handler(std::string body) {
    try {
        return nlohmann::json::parse(std::move(body));
    } catch (const std::exception& e) {
        throw std::runtime_error(
            std::string("response body is not JSON: ") + e.what());
    }
}

/**
 * @brief One bounded exchange: connect, send, read the whole response,
 *        status-check, decode via @p handler.
 *
 * The whole-body single-shot the provider-info family rides on. The response
 * is read completely (Beast's default body limit applies) before the handler
 * runs; a non-200 reply is a failure whose body (bounded to 2048 bytes) is
 * folded into the message, exactly as the direct http_request overload's
 * no-gating policy intends for provider error payloads.
 *
 * @tparam Handler A callable `Product(std::string)` invoked once per
 *                 successful (200) exchange with the response body moved in.
 *                 Must be copyable/stateless — the fetch engine reuses one
 *                 handler across retry attempts. json_handler is the stock
 *                 one.
 * @param executor         Executor the connect and exchange run on.
 * @param endpoint         Where to connect, as resolve_endpoint parsed it.
 * @param request          The fully built request (method, target, headers);
 *                         re-sent verbatim by the retry engine.
 * @param handler          The body→product decoder for a 200 body.
 * @param read_timeout_sec Response read deadline; 0 waits indefinitely.
 * @return The handler's product (json_handler → nlohmann::json).
 * @throws HttpRequestException{Stage::Connect} on connect failure (from
 *         connect()); Stage::HandleResponse for a non-200 reply (status +
 *         folded body) or a handler fault wrapped with the request context
 *         (a non-JSON body via json_handler, a stray custom-handler throw);
 *         the underlying HttpRequestTimeoutException for a slow backend.
 */
template<typename Handler>
boost::asio::awaitable<std::invoke_result_t<Handler, std::string>>
fetch_once(
    boost::asio::any_io_executor executor,
    ResolvedEndpoint endpoint,
    ModelRequestInterpreter::HttpRequest request,
    Handler handler,
    std::size_t read_timeout_sec = DEFAULT_HTTP_READ_TIMEOUT_SEC)
{
    namespace http = boost::beast::http;

    // The request moves into the exchange below; keep the log context for the
    // status-gate and handler-fault paths, which outlive it.
    const std::string method(request.method_string());
    const std::string target(request.target());
    const std::string host(request[http::field::host]);

    connection_stream stream = co_await connect(executor, endpoint);

    // The direct bounded overload: no status gating, whole response back —
    // the caller wants to render non-200 bodies itself, like here.
    http::response<http::string_body> response =
        co_await http_request(std::move(stream), std::move(request), read_timeout_sec);

    if (response.result() != http::status::ok) {
        constexpr std::size_t kMaxErrorBody = 2048;
        std::string body = response.body();
        if (body.size() > kMaxErrorBody) body.resize(kMaxErrorBody);
        std::string message = "request rejected";
        if (!body.empty()) message += ": " + body;
        throw HttpRequestException(
            HttpRequestException::Stage::HandleResponse,
            std::move(message), {}, method, target, host,
            response.result_int());
    }

    try {
        co_return handler(std::move(response.body()));
    } catch (const HttpRequestException&) {
        // A custom handler that already raised the module's lifecycle
        // exception keeps it unchanged — fetch_once never double-wraps.
        throw;
    } catch (const std::exception& e) {
        throw HttpRequestException(
            HttpRequestException::Stage::HandleResponse,
            std::string("response handler failed: ") + e.what(),
            {}, method, target, host);
    }
}

/**
 * @brief JSON convenience overload of fetch_once: no handler, returns
 *        nlohmann::json.
 *
 * Equivalent to fetch_once(..., json_handler, read_timeout_sec). The
 * provider-info queries use this shape — one GET, the body parsed as JSON.
 */
inline boost::asio::awaitable<nlohmann::json> fetch_once(
    boost::asio::any_io_executor executor,
    ResolvedEndpoint endpoint,
    ModelRequestInterpreter::HttpRequest request,
    std::size_t read_timeout_sec = DEFAULT_HTTP_READ_TIMEOUT_SEC)
{
    co_return co_await fetch_once(
        std::move(executor), std::move(endpoint), std::move(request),
        json_handler, read_timeout_sec);
}

/**
 * @brief fetch_once with plain retry, as a callable object: the bounded
 *        counterpart of the streaming complete engine.
 *
 * A stateful retry engine bound to one executor and one retry policy —
 * construct it once and call it per query:
 *
 *     endpoint::fetch fetch_model{io.get_executor()};
 *     nlohmann::json catalogue = co_await fetch_model(
 *         resolved_endpoint, built_request);
 *
 * One call is up to _max_retry_attempts + 1 fetch_once exchanges: the INITIAL
 * exchange plus, while failures classify as recoverable, one retry each (the
 * initial exchange is NOT counted against the retry budget). Every attempt is
 * a fresh connect, the SAME @p request re-sent verbatim, and the SAME
 * @p handler re-invoked — keep handlers stateless, as json_handler is.
 *
 * An attempt SUCCEEDS when fetch_once returns (a 200 body decoded); any
 * HttpRequestException is classified by the shared retry_policy::_recoverable
 * verdict: a recoverable one backs off and retries to the budget, a
 * non-recoverable one propagates immediately. A non-HTTP exception (a
 * handler's stray throw) is the caller's own bug and surfaces without retry.
 * The final failure propagates as-is, with the give-up recorded in the log.
 *
 * Concurrency: ONE operator() in flight per instance — the retry state
 * (_backoff, _timer) is shared and unsynchronized. Run concurrent queries on
 * separate instances (they share nothing but the executor).
 */
class fetch : public retry_policy {
public:
    using retry_policy::retry_policy;

    /**
     * @brief Run one bounded query with plain retry.
     *
     * @tparam Handler The body→product decoder, reused every attempt; deduced.
     * @param endpoint         Where to connect, every attempt.
     * @param request          The fully built request, re-sent verbatim every
     *                         attempt; building it is the caller's concern.
     * @param handler          The decoder for a 200 body, every attempt.
     * @param read_timeout_sec Read deadline, forwarded to fetch_once.
     * @return The handler's product of the first attempt that succeeded.
     */
    template<typename Handler>
    boost::asio::awaitable<std::invoke_result_t<Handler, std::string>>
    operator()(
        ResolvedEndpoint endpoint,
        ModelRequestInterpreter::HttpRequest request,
        Handler handler,
        std::size_t read_timeout_sec = DEFAULT_HTTP_READ_TIMEOUT_SEC)
    {
        // Attempt 0 is the initial exchange; 1.._max_retry_attempts are the
        // retries — the initial request is NOT counted against the budget.
        for (unsigned int attempt = 0; attempt <= _max_retry_attempts; ++attempt) {
            try {
                co_return co_await fetch_once(
                    _executor, endpoint, request, handler, read_timeout_sec);
            } catch (const HttpRequestException& failure) {
                if (!_recoverable(failure) || attempt == _max_retry_attempts) {
                    // The verdict is final and the failure — the report —
                    // propagates untouched; this line is the retry layer's
                    // own record of giving up, with the budget spent.
                    logging::Logger::error(
                        "fetch failed, giving up after "
                        + std::to_string(attempt + 1) + " of "
                        + std::to_string(_max_retry_attempts + 1)
                        + " attempts: " + failure.to_string());
                    throw;
                }
                // Recoverable, budget remains: one line before the backoff
                // so the retry is visible in the log.
                logging::Logger::info(
                    "transient failure, fetch attempt "
                    + std::to_string(attempt + 1) + " of "
                    + std::to_string(_max_retry_attempts + 1)
                    + " failed, retrying: " + failure.to_string());
            } catch (const std::exception& failure) {
                // A non-HTTP exception out of fetch_once is a handler fault —
                // the caller's own bug, never retry material.
                logging::Logger::error(
                    std::string("fetch failed with a non-HTTP exception, "
                                "not retrying: ") + failure.what());
                throw;
            } catch (...) {
                logging::Logger::error(
                    "fetch failed with an unknown exception, not retrying");
                throw;
            }

            co_await _sleep();
        }

        // Unreachable: every path out of the last iteration returns or
        // throws.
        throw std::logic_error("endpoint::fetch: retry loop fell through");
    }

    /**
     * @brief JSON convenience overload: no handler, returns nlohmann::json.
     *
     * Equivalent to operator()(..., json_handler, read_timeout_sec).
     */
    boost::asio::awaitable<nlohmann::json> operator()(
        ResolvedEndpoint endpoint,
        ModelRequestInterpreter::HttpRequest request,
        std::size_t read_timeout_sec = DEFAULT_HTTP_READ_TIMEOUT_SEC)
    {
        co_return co_await (*this)(
            std::move(endpoint), std::move(request), json_handler, read_timeout_sec);
    }
};

} // namespace endpoint
