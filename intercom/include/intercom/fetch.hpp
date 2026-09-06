#pragma once

//
// fetch.hpp — the bounded single-shot WebSocket request API: fetch_once and
// the fetch retry engine
// ====================================================================
//
// The intercom counterpart of endpoint::fetch, one protocol up: ONE WebSocket
// session per attempt — connect + upgrade, send one TEXT message, read one
// complete reply, close — and the reply comes back as a plain std::string.
// No handler abstraction: the module is transport-only, so what goes out and
// what comes back are both opaque byte strings (JSON in practice). The
// default return type is therefore std::string, not nlohmann::json.
//
// Layering mirrors the HTTP side, one level lower each time:
//
//   connect_websocket(executor, resolved)  — connection + upgrade + Connect
//                                            folding (websocket_stream.hpp)
//   fetch_once(...)                        — ONE bounded exchange: connect,
//                                            send one message, read one reply,
//                                            close, return the reply bytes.
//   fetch                                  — fetch_once plus a retry policy:
//                                            on a recoverable failure the SAME
//                                            message is re-sent verbatim, up
//                                            to the retry budget.
//
// The retry engine inherits endpoint::retry_policy for its backoff/timer
// machinery (the type-agnostic half) but does NOT reuse its HttpRequestException
// verdict — intercom failures are WsException, so the recoverability table is
// the module-local is_recoverable below, keyed on WsException.

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/websocket.hpp>

#include "endpoint/model_request.hpp"   // ResolvedEndpoint
#include "endpoint/retry_policy.hpp"    // the shared backoff/timer base
#include "intercom/websocket_stream.hpp"
#include "intercom/ws_exception.hpp"

#include "logging/logger.hpp"

namespace intercom {

/// Deadline for sending one message. Mirrors endpoint's write deadline: a
/// large JSON payload on a slow link is not an unreachable service.
inline constexpr std::size_t DEFAULT_WS_WRITE_TIMEOUT_SEC = 60;
/// Deadline for reading one reply. Generous by default — an internal backend
/// may compute before answering — and per-call configurable (0 = indefinite).
inline constexpr std::size_t DEFAULT_WS_READ_TIMEOUT_SEC = 300;

/**
 * @brief One bounded WebSocket exchange: connect, send one message, read one
 *        reply, close, return the reply bytes.
 *
 * The single-shot the module's internal queries ride on. The reply is read as
 * ONE complete message (fragmentation reassembled) before the function
 * returns; the frame opcode the peer used is irrelevant — the payload comes
 * back as a std::string either way.
 *
 * @param executor         Executor the connect and exchange run on.
 * @param endpoint         Where to connect (host/port/tls; endpoint.target is
 *                         the WebSocket path), as endpoint::resolve_endpoint
 *                         parsed it — or constructed directly.
 * @param request          The message to send (a TEXT frame, JSON in
 *                         practice); re-sent verbatim by the retry engine.
 * @param read_timeout_sec Reply read deadline; 0 waits indefinitely.
 * @return The reply message's payload bytes.
 * @throws WsException{Stage::Connect} on connect/upgrade failure (from
 *         connect_websocket); Stage::Write / Stage::Read for a mid-exchange
 *         transport fault; WsTimeoutException for a slow backend. An empty
 *         stream surfaces as WsException{Stage::Unknown}.
 */
inline boost::asio::awaitable<std::string> fetch_once(
    boost::asio::any_io_executor executor,
    endpoint::ResolvedEndpoint endpoint,
    std::string request,
    std::size_t read_timeout_sec = DEFAULT_WS_READ_TIMEOUT_SEC)
{
    websocket_stream stream = co_await connect_websocket(executor, endpoint);

    // The exchange below consumes the request; keep the endpoint context for
    // the error paths, which outlive it.
    const std::string host = endpoint.host;
    const std::string target = endpoint.target;

    WsException::Stage stage = WsException::Stage::Write;
    try {
        // The write deadline is deliberately longer than the connect one: a
        // large message on a slow uplink is not an unreachable service.
        stream.expires_after(
            std::chrono::seconds(DEFAULT_WS_WRITE_TIMEOUT_SEC));
        co_await stream.write(std::move(request));

        stage = WsException::Stage::Read;
        if (read_timeout_sec == 0) {
            stream.expires_never();
        } else {
            stream.expires_after(std::chrono::seconds(read_timeout_sec));
        }
        boost::beast::flat_buffer buffer;
        co_await stream.read(buffer);

        std::string reply = boost::beast::buffers_to_string(buffer.data());
        co_await stream.close();
        co_return reply;
    } catch (const boost::system::system_error& exception) {
        // A deadline firing during the reply read is the read-timeout case:
        // report it with the dedicated type so callers can distinguish a slow
        // backend. (A write-phase timeout keeps the generic path.)
        if (stage == WsException::Stage::Read &&
            (exception.code() == boost::beast::error::timeout ||
             exception.code() == boost::asio::error::timed_out)) {
            throw WsTimeoutException(
                std::string("reply read timed out after ") +
                    std::to_string(read_timeout_sec) + "s: " +
                    exception.what(),
                exception.code(), host, target);
        }
        throw WsException(
            stage,
            std::string("websocket exchange failed: ") + exception.what(),
            exception.code(), host, target);
    } catch (const WsException&) {
        // Preserve failures already enriched (empty stream, connect failure).
        throw;
    } catch (const std::exception& exception) {
        throw WsException(
            stage,
            std::string("websocket exchange exception: ") + exception.what(),
            {}, host, target);
    }
}

/**
 * @brief The intercom recoverability verdict for one failed attempt.
 *
 * The module's own table, keyed on WsException (endpoint::retry_policy's
 * default verdict is HttpRequestException-typed and does not apply here). A
 * recoverable failure is one a FRESH attempt could plausibly fix:
 *
 *   * Connect failures classify by error-code category: DNS try-again,
 *     refused / reset / timed-out, the stream-level connect timeout, and a
 *     truncated TLS handshake are transient; authoritative host-not-found and
 *     certificate-verification failures are not. An upgrade rejection
 *     (websocket::error::upgrade_declined, bad_response, ...) is also NOT
 *     recoverable — the same request draws the same answer.
 *   * Write / Read mid-exchange (a dropped session, a read timeout — the
 *     WsTimeoutException included) — recoverable: the request itself was
 *     sound.
 *   * Unknown — NOT: something unwrapped and unclassifiable; fail fast.
 */
inline bool is_recoverable(const WsException& failure) noexcept {
    namespace asio = boost::asio;
    namespace ssl = asio::ssl;

    switch (failure.stage()) {
        case WsException::Stage::Connect: {
            const auto& ec = failure.error_code();
            if (ec.category() == asio::error::get_netdb_category()) {
                // Authoritative not-found is a config bug; TRY_AGAIN is the
                // resolver being briefly unable to answer.
                return ec == asio::error::host_not_found_try_again;
            }
            if (ec == asio::error::connection_refused ||
                ec == asio::error::connection_reset ||
                ec == asio::error::timed_out ||
                ec == boost::beast::error::timeout) {
                return true;
            }
            if (ec.category() == ssl::error::get_stream_category()) {
                // A handshake cut mid-stream may be a middlebox hiccup; a
                // certificate the client rejects never becomes valid.
                return ec == ssl::error::stream_truncated;
            }
            return false;   // upgrade_declined, host_not_found, cert, ...
        }
        case WsException::Stage::Write:
        case WsException::Stage::Read:
            return true;
        case WsException::Stage::Unknown:
        default:
            return false;
    }
}

/**
 * @brief fetch_once with plain retry, as a callable object: the bounded
 *        WebSocket counterpart of endpoint::fetch.
 *
 * A stateful retry engine bound to one executor and one retry policy —
 * construct it once and call it per query:
 *
 *     intercom::fetch fetch_internal{io.get_executor()};
 *     std::string reply = co_await fetch_internal(resolved_endpoint, "{}");
 *
 * One call is up to _max_retry_attempts + 1 fetch_once exchanges: the INITIAL
 * exchange plus, while failures classify as recoverable (is_recoverable),
 * one retry each. Every attempt is a fresh connect/upgrade, the SAME message
 * re-sent verbatim. An attempt SUCCEEDS when fetch_once returns; a
 * non-recoverable failure propagates immediately; a non-WsException (a stray
 * std::exception) is the caller's own bug and surfaces without retry. The
 * final failure propagates as-is, with the give-up recorded in the log.
 *
 * Concurrency: ONE operator() in flight per instance — the retry state
 * (_backoff, _timer) is shared and unsynchronized. Run concurrent queries on
 * separate instances.
 */
class fetch : public endpoint::retry_policy {
public:
    using endpoint::retry_policy::retry_policy;

    /**
     * @brief Run one bounded WebSocket query with plain retry.
     *
     * @param endpoint         Where to connect, every attempt.
     * @param request          The message, re-sent verbatim every attempt.
     * @param read_timeout_sec Read deadline, forwarded to fetch_once.
     * @return The reply bytes of the first attempt that succeeded.
     */
    boost::asio::awaitable<std::string> operator()(
        endpoint::ResolvedEndpoint endpoint,
        std::string request,
        std::size_t read_timeout_sec = DEFAULT_WS_READ_TIMEOUT_SEC)
    {
        // Per-call reset: the shared retry state must not leak a previous
        // call's exhausted backoff into this one.
        _backoff = _initial_backoff;

        // Attempt 0 is the initial exchange; 1.._max_retry_attempts are the
        // retries — the initial request is NOT counted against the budget.
        for (unsigned int attempt = 0; attempt <= _max_retry_attempts; ++attempt) {
            try {
                co_return co_await fetch_once(
                    _executor, endpoint, request, read_timeout_sec);
            } catch (const WsException& failure) {
                if (!is_recoverable(failure) || attempt == _max_retry_attempts) {
                    logging::Logger::error(
                        "intercom fetch failed, giving up after "
                        + std::to_string(attempt + 1) + " of "
                        + std::to_string(_max_retry_attempts + 1)
                        + " attempts: " + failure.to_string());
                    throw;
                }
                logging::Logger::info(
                    "transient failure, intercom fetch attempt "
                    + std::to_string(attempt + 1) + " of "
                    + std::to_string(_max_retry_attempts + 1)
                    + " failed, retrying: " + failure.to_string());
            } catch (const std::exception& failure) {
                // A non-WS exception out of fetch_once is a caller's own bug,
                // never retry material.
                logging::Logger::error(
                    std::string("intercom fetch failed with a non-WS "
                                "exception, not retrying: ") + failure.what());
                throw;
            } catch (...) {
                logging::Logger::error(
                    "intercom fetch failed with an unknown exception, not retrying");
                throw;
            }

            co_await _sleep();
        }

        // Unreachable: every path out of the last iteration returns or throws.
        throw std::logic_error("intercom::fetch: retry loop fell through");
    }
};

} // namespace intercom
