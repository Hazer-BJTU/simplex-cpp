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
// Retry semantics (see is_recoverable and the fetch constructor):
//
//   * Connect-stage failures are retried when the transport error is an
//     unambiguous transient (refused/reset/timeout/try-again/truncated TLS) —
//     safe, because the request never reached the server.
//   * Write/Read failures are AMBIGUOUS: the server may already have
//     processed the request and lost the reply. They are retried ONLY when
//     the caller declared the operation idempotent (fetch's constructor flag)
//     AND the error is an unambiguous transient transport failure — never
//     cancellation, never a WebSocket protocol/close error.

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

/// Deadline for reading one reply, applied as the WebSocket idle timeout
/// (no-activity limit). Generous by default — an internal backend may compute
/// before answering — and per-call configurable (0 = indefinite).
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
 * The read deadline is applied as Beast's idle timeout (set_option), NOT as a
 * lowest-layer tcp_stream deadline: the underlying tcp_stream timer is
 * disabled by connect_websocket, per Beast's requirement. 0 disables the
 * timeout (wait indefinitely).
 *
 * @param executor         Executor the connect and exchange run on.
 * @param endpoint         Where to connect (host/port/tls; endpoint.target is
 *                         the WebSocket path), as endpoint::resolve_endpoint
 *                         parsed it (ws:// wss:// http:// https://) — or
 *                         constructed directly.
 * @param request          The message to send (a TEXT frame, JSON in
 *                         practice); re-sent verbatim by the retry engine.
 * @param read_timeout_sec Reply read deadline (idle timeout); 0 waits
 *                         indefinitely.
 * @param context          TLS client context; wss:// flavour only, global by
 *                         default — pass a custom context for a private CA.
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
    std::size_t read_timeout_sec = DEFAULT_WS_READ_TIMEOUT_SEC,
    endpoint::ssl_context& context = endpoint::get_global_ssl_context())
{
    namespace websocket = boost::beast::websocket;

    websocket_stream stream = co_await connect_websocket(executor, endpoint, context);

    // The exchange below consumes the request; keep the endpoint context for
    // the error paths, which outlive it.
    const std::string host = endpoint.host;
    const std::string target = endpoint.target;

    WsException::Stage stage = WsException::Stage::Write;
    try {
        // Configure the session timeout through Beast's own option: the reply
        // read deadline as the idle timeout. This also bounds a stuck write
        // on a dead session — the connection is closed after no activity for
        // the idle interval, failing whichever operation is pending.
        auto timeout = websocket::stream_base::timeout::suggested(
            boost::beast::role_type::client);
        timeout.handshake_timeout =
            std::chrono::seconds(endpoint::DEFAULT_TIMEOUT_SEC);
        timeout.idle_timeout = (read_timeout_sec == 0)
            ? websocket::stream_base::none()
            : std::chrono::seconds(read_timeout_sec);
        stream.set_option(timeout);

        co_await stream.write(std::move(request));

        stage = WsException::Stage::Read;
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
 * @brief A Connect-stage error is transient: a fresh attempt could succeed.
 *
 * The unambiguous half of the retry verdict — the request never reached the
 * server, so re-sending is always safe when the failure is a network
 * transient. DNS try-again, refused / reset / timed-out, the connect timeout,
 * and a truncated TLS handshake qualify; authoritative host-not-found,
 * certificate-verification failure, and upgrade rejection
 * (websocket::error::upgrade_declined, ...) do not.
 */
inline bool is_transient_connect_error(
    const boost::system::error_code& ec) noexcept
{
    namespace asio = boost::asio;
    namespace ssl = asio::ssl;

    if (ec.category() == asio::error::get_netdb_category()) {
        return ec == asio::error::host_not_found_try_again;
    }
    if (ec == asio::error::connection_refused ||
        ec == asio::error::connection_reset ||
        ec == asio::error::timed_out ||
        ec == boost::beast::error::timeout) {
        return true;
    }
    if (ec.category() == ssl::error::get_stream_category()) {
        return ec == ssl::error::stream_truncated;
    }
    return false;
}

/**
 * @brief A Write/Read error is an unambiguous transient transport failure.
 *
 * An explicit whitelist, not a stage-wide "recoverable": only these errors
 * justify re-sending a request. Everything else — cancellation
 * (operation_aborted), WebSocket protocol errors, application/peer close
 * conditions, message-too-big — is NOT recoverable, because retrying cannot
 * help and (for cancellation) would transform the caller's request to stop
 * into another network attempt.
 */
inline bool is_transient_transport_error(
    const boost::system::error_code& ec) noexcept
{
    namespace asio = boost::asio;

    return ec == asio::error::connection_reset ||
           ec == asio::error::eof ||
           ec == asio::error::broken_pipe ||
           ec == asio::error::timed_out ||
           ec == boost::beast::error::timeout;
}

/**
 * @brief The intercom recoverability verdict for one failed attempt.
 *
 * The module's own table, keyed on WsException (endpoint::retry_policy's
 * default verdict is HttpRequestException-typed and does not apply here).
 *
 *   * Connect — recoverable iff the transport error is a transient (see
 *     is_transient_connect_error). Safe to retry unconditionally: the request
 *     never left the client.
 *   * Write / Read — AMBIGUOUS: the server may have processed the request
 *     before the reply was lost. Recoverable ONLY when @p idempotent is true
 *     (the caller declared the operation safe to re-execute) AND the error is
 *     an unambiguous transient transport failure (see
 *     is_transient_transport_error). A non-idempotent operation therefore
 *     never re-sends after the request may have been acted on.
 *   * Unknown — never.
 *
 * @param idempotent Whether the caller declared the request safe to re-send
 *                   after an ambiguous Write/Read failure.
 */
inline bool is_recoverable(const WsException& failure, bool idempotent) noexcept {
    switch (failure.stage()) {
        case WsException::Stage::Connect:
            return is_transient_connect_error(failure.error_code());
        case WsException::Stage::Write:
        case WsException::Stage::Read:
            return idempotent &&
                   is_transient_transport_error(failure.error_code());
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
 * re-sent verbatim.
 *
 * The retry contract (see is_recoverable):
 *   * Connect-stage transient failures are always retried — safe, the request
 *     never reached the server.
 *   * Write/Read failures are retried ONLY when this engine was constructed
 *     with idempotent = true AND the error is an unambiguous transient
 *     transport failure. An idempotent request is one the caller declares
 *     safe to re-execute (e.g. a read-only query); for anything with a side
 *     effect (create/start/update/commit), leave idempotent false and the
 *     ambiguous failure propagates immediately, so the caller can dedupe or
 *     report it. Cancellation and protocol errors are never retried.
 *
 * Concurrency: ONE operator() in flight per instance — the retry state
 * (_backoff, _timer) is shared and unsynchronized. Run concurrent queries on
 * separate instances.
 */
class fetch : public endpoint::retry_policy {
public:
    /**
     * @brief Construct the retry engine for one executor.
     *
     * @param executor            Executor every attempt and backoff runs on.
     * @param initial_backoff     Backoff before the FIRST retry; doubles per
     *                            retry up to max_backoff.
     * @param max_backoff         Backoff ceiling.
     * @param max_retry_attempts  Maximum number of RETRIES after the initial
     *                            attempt (the initial attempt does not count).
     * @param idempotent          Whether the request may be safely re-sent
     *                            after an ambiguous Write/Read failure (see
     *                            the class doc). Default false.
     */
    explicit fetch(
        boost::asio::any_io_executor executor,
        std::chrono::milliseconds initial_backoff = std::chrono::milliseconds{500},
        std::chrono::milliseconds max_backoff = std::chrono::milliseconds{120000},
        unsigned int max_retry_attempts = 3,
        bool idempotent = false)
        : endpoint::retry_policy(
              std::move(executor), initial_backoff, max_backoff,
              max_retry_attempts),
          _idempotent(idempotent)
    {}

    /**
     * @brief Run one bounded WebSocket query with plain retry.
     *
     * @param endpoint         Where to connect, every attempt.
     * @param request          The message, re-sent verbatim every attempt.
     * @param read_timeout_sec Read deadline, forwarded to fetch_once.
     * @param context          TLS client context; wss:// flavour only,
     *                         forwarded to fetch_once.
     * @return The reply bytes of the first attempt that succeeded.
     */
    boost::asio::awaitable<std::string> operator()(
        endpoint::ResolvedEndpoint endpoint,
        std::string request,
        std::size_t read_timeout_sec = DEFAULT_WS_READ_TIMEOUT_SEC,
        endpoint::ssl_context& context = endpoint::get_global_ssl_context())
    {
        // Per-call reset: the shared retry state must not leak a previous
        // call's exhausted backoff into this one.
        _backoff = _initial_backoff;

        // Attempt 0 is the initial exchange; 1.._max_retry_attempts are the
        // retries — the initial request is NOT counted against the budget.
        for (unsigned int attempt = 0; attempt <= _max_retry_attempts; ++attempt) {
            try {
                co_return co_await fetch_once(
                    _executor, endpoint, request, read_timeout_sec, context);
            } catch (const WsException& failure) {
                if (!is_recoverable(failure, _idempotent) ||
                    attempt == _max_retry_attempts) {
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

private:
    bool _idempotent;
};

} // namespace intercom
