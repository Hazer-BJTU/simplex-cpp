#pragma once

//
// complete.hpp — one model invocation, end to end: connect, exchange, assemble
// ====================================================================
//
// The whole-exchange convenience over the canonical wiring (see the
// model_request.hpp header block): complete_once connects to the resolved
// endpoint, runs ONE driver exchange as a spawned producer task, drains the
// reader's consumer side inline, and hands back the reader's assembled
// model_io::MessageItem.
//
// Failure handling splits by side:
//   * Connect failures (the system_error family out of
//     create_connection_stream) are rethrown to the caller as
//     HttpRequestException{Stage::Connect}, keeping the transport error code
//     and the endpoint context. Nothing was spawned yet, so no producer side
//     exists to tear down.
//   * The spawned producer never lets an exception escape: pump() has already
//     finished the handler ERROR-side before rethrowing — which unblocks the
//     consumer below — so the failure is rendered into the log only, and the
//     exchange's outcome reaches this coroutine through the consumer's end
//     state instead.
//

#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>
#include <concepts>

#include <boost/asio.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/core/error.hpp>

#include "endpoint/http_request_exception.hpp"
#include "endpoint/model_request.hpp"

#include "logging/logger.hpp"

namespace endpoint {

// ---- retry classification -----------------------------------------------------

/**
 * @brief The default recoverability verdict for one failed exchange.
 *
 * A recoverable failure is one a FRESH exchange could plausibly fix: server
 * overload, a transient network state, a truncated stream. The verdict is
 * made on the HttpRequestException's own fields — status first (the
 * provider's answer, when there is one), then the stage/error-code pair:
 *
 *   * HTTP status 429 / 408 / 5xx — recoverable (back off and retry).
 *   * any other status (401, 403, 400, 404, 422, ...) — NOT: the same
 *     request will draw the same answer; retrying only re-bills tokens.
 *   * Connect failures classify by error-code category: DNS try-again,
 *     refused / reset / timed-out, the stream-level connect timeout, and a
 *     truncated TLS handshake are transient; authoritative host-not-found
 *     and certificate verification failures are not (retrying cannot fix
 *     either).
 *   * Write/Read mid-exchange (a dropped connection, a read timeout — the
 *     bounded driver's HttpRequestTimeoutException included) — recoverable:
 *     the request itself was sound.
 *   * HandleResponse with no status — recoverable: a decode fault the retry
 *     budget absorbs (a genuinely changed wire protocol exhausts it fast).
 *   * CreateRequest / Unknown — NOT: our own request-building bug, or
 *     something unwrapped and unclassifiable; fail fast.
 *
 * Providers disagree at the margins; override per deployment via
 * RetryPolicy::is_recoverable.
 */
inline bool is_recoverable_by_default(const HttpRequestException& failure)
{
    namespace asio = boost::asio;
    namespace ssl = asio::ssl;

    const unsigned status = failure.status();
    if (status != 0) {
        if (status == 429 || status == 408) return true;
        return status >= 500 && status <= 599;
    }

    using Stage = HttpRequestException::Stage;
    switch (failure.stage()) {
        case Stage::Connect: {
            const auto& ec = failure.error_code();
            if (ec.category() == asio::error::get_netdb_category()) {
                // Authoritative not-found is a config bug; TRY_AGAIN is
                // the resolver being briefly unable to answer.
                return ec == asio::error::host_not_found_try_again;
            }
            if (ec == asio::error::connection_refused
                || ec == asio::error::connection_reset
                || ec == asio::error::timed_out
                || ec == boost::beast::error::timeout) {
                return true;
            }
            if (ec.category() == ssl::error::get_stream_category()) {
                // A handshake cut mid-stream may be a middlebox hiccup;
                // a certificate the client rejects never becomes valid.
                return ec == ssl::error::stream_truncated;
            }
            return false;
        }
        case Stage::Write:
        case Stage::Read:
        case Stage::HandleResponse:
            return true;
        case Stage::CreateRequest:
        case Stage::Unknown:
        default:
            return false;
    }
}

/**
 * @brief The knobs of complete()'s plain-retry loop.
 *
 * Plain retry: on a recoverable failure the WHOLE exchange is discarded and
 * re-read from scratch (fresh connect, fresh reader) — most providers offer
 * no stream resume, so a full re-read is the only resumption there is. The
 * budget deliberately counts ATTEMPTS, not failures: a retry re-sends the
 * request and re-bills its tokens, so the default is small.
 */
struct RetryPolicy {
    /// Total exchange attempts, first one included; 0 is treated as 1.
    unsigned max_attempts = 3;

    /// Delay before the 2nd attempt; doubles per attempt up to max_backoff.
    /// No jitter by default (deterministic tests); thread randomness through
    /// a custom backoff here if fleet-level thundering herds matter.
    std::chrono::milliseconds initial_backoff{500};
    std::chrono::milliseconds max_backoff{8'000};

    /// Per-deployment override of is_recoverable_by_default; empty uses the
    /// default. Receives every failure complete() would retry.
    std::function<bool(const HttpRequestException&)> is_recoverable;

    /// Delay before attempt N (1-based); replace to inject jitter or to
    /// honor a provider's Retry-Once semantics.
    std::function<std::chrono::milliseconds(unsigned attempt)> backoff;
};

// ---- the plain-retry whole-exchange convenience --------------------------------

/**
 * @brief complete_once with plain retry: on a recoverable failure, reset the
 *        reader and re-read the WHOLE response from scratch.
 *
 * One attempt is one complete_once: fresh connect, fresh request (from @p
 * request_builder — rebuilt per attempt so time-stamped or idempotency-keyed
 * requests do not go stale), and the SAME @p reader reset to its
 * just-constructed state via clear() — accumulators, end-state flags, and the
 * handler's framing buffer and channel all rewind, so no per-attempt factory
 * is needed. Hooks registered on the reader SURVIVE the reset and observe
 * every attempt's deltas (a monitor pre-wires once; note it when counting).
 * An attempt SUCCEEDS only on the reader's Completed end state; every other
 * outcome is classified for retry:
 *
 *   * end_error() set — the producer-side exception complete_once already
 *     logged; rethrown here and classified by the policy (a non-recoverable
 *     one propagates immediately, no further attempts).
 *   * Faulted with no end_error() — the stream ended truncated (server
 *     close without the terminal event): recoverable by design, retried to
 *     the budget, then reported as HttpRequestException{Read}.
 *   * Aborted — the consumer called reader->abort() (a deadline firing, a
 *     caller backing out): NOT retried, however many attempts remain;
 *     surfaces as HttpRequestException{Unknown} with the request context.
 *
 * Connect failures propagate out of complete_once directly and classify the
 * same way. A hook/_accumulate fault inside consume() is the caller's own
 * bug and passes straight through — no retry, no wrapping.
 *
 * The final failure propagates as-is (the last attempt's exception, or the
 * truncation/abort wrapping above); earlier attempts are only in the log,
 * where complete_once already rendered them.
 *
 * @tparam Delta          The reader's decoded event type.
 * @tparam Driver         The request driver per exchange; deduced, must be
 *                        copyable (reused across attempts — keep drivers
 *                        stateless, as the stock ones are).
 * @tparam RequestBuilder Nullary callable returning a fresh
 *                        ModelRequestInterpreter::HttpRequest; called once
 *                        per attempt.
 * @param executor        Executor every attempt and backoff runs on.
 * @param model_endpoint  Where to connect, every attempt.
 * @param request_builder Builds each attempt's request.
 * @param reader          The consumer half, REUSED across attempts: clear()
 *                        rewinds it (and its handler) before every attempt,
 *                        so hand in a fresh one or a previously drained one.
 * @param driver          The exchange driver, every attempt.
 * @param policy          Retry budget and classification; defaults apply.
 * @return The assembled model_io::MessageItem of the first attempt that
 *         reached the terminal event.
 */
template<
    typename Delta,
    RequestDriver<typename ModelResponseReader<Delta>::Handler> Driver,
    typename RequestBuilder
>
boost::asio::awaitable<model_io::MessageItem> complete(
    boost::asio::any_io_executor executor,
    ResolvedEndpoint model_endpoint,
    RequestBuilder request_builder,
    std::shared_ptr<ModelResponseReader<Delta>> reader,
    Driver driver,
    RetryPolicy policy = {})
{
    const unsigned attempts =
        policy.max_attempts == 0 ? 1 : policy.max_attempts;
    const auto recoverable = [&](const HttpRequestException& failure) {
        return policy.is_recoverable
                   ? policy.is_recoverable(failure)
                   : is_recoverable_by_default(failure);
    };
    const auto backoff_before = [&](unsigned attempt) {
        if (policy.backoff) return policy.backoff(attempt);
        // Exponential with a cap; the shift is clamped so huge budgets
        // cannot overflow the milliseconds.
        const unsigned shift = attempt > 11 ? 11 : attempt - 1;
        const auto scaled = policy.initial_backoff * (1 << shift);
        return scaled > policy.max_backoff ? policy.max_backoff : scaled;
    };

    for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
        auto request = request_builder();
        // The reuse reset — before EVERY attempt (the first included), so a
        // handed-in reader's prior life cannot leak into attempt 1: the
        // end-state flags, the post-mortem exception, the accumulators, and
        // the handler's framing buffer + channel (rebuilt) all rewind. The
        // producer of the previous attempt has exited by here (complete_once
        // returned), and its spawned wrapper touches nothing shared after
        // pump's finish, so the reset cannot race it.
        reader->clear();

        bool self_aborted = false;
        try {
            auto item = co_await complete_once<Delta>(
                executor, model_endpoint, request, reader, driver);

            const auto end = reader->end_state();
            if (end == ModelResponseReader<Delta>::EndState::Completed) {
                co_return item;
            }

            if (end == ModelResponseReader<Delta>::EndState::Aborted) {
                // The consumer asked out; never turn that into a retry —
                // the flag below bypasses classification entirely, so even
                // a permissive policy cannot outvote the caller.
                self_aborted = true;
                throw wrap_request_failure(
                    HttpRequestException::Stage::Unknown,
                    "stream aborted by the consumer before the terminal event",
                    {}, request);
            }
            if (reader->end_error()) {
                // complete_once already logged it; surface for classification.
                std::rethrow_exception(reader->end_error());
            }
            // Faulted with no producer exception: truncated without the
            // terminal event — the case plain retry exists for.
            throw wrap_request_failure(
                HttpRequestException::Stage::Read,
                "stream ended without the terminal event", {}, request);
        } catch (const HttpRequestException& failure) {
            // Non-recoverable, or the budget is spent: propagate the last
            // failure untouched (its stage/status/context is the report).
            if (self_aborted || !recoverable(failure) || attempt == attempts) {
                throw;
            }
        }

        boost::asio::steady_timer timer(executor);
        timer.expires_after(backoff_before(attempt + 1));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }

    // Unreachable: the loop returns, or its last iteration threw.
    throw std::logic_error("endpoint::complete: retry loop fell through");
}

/**
 * @brief Run one complete exchange against @p model_endpoint and return the
 *        assembled response.
 *
 * Connect first (its failures propagate as HttpRequestException{Stage::Connect}
 * carrying the transport error code and the endpoint's target/host context),
 * then the canonical two-sided wiring: the producer — @p driver pumped over a
 * freshly connected connection_stream — runs as a co_spawned task whose
 * exceptions are LOG-ONLY (pump has already torn the channel down for the
 * consumer by the time they are caught; see request.hpp's sse_request for the
 * exception contract), while this coroutine drains @p response_reader->consume()
 * inline and returns the assembled record. On a stream that ends without the
 * terminal event (a logged transport fault, a cooperative stop, a server that
 * closed early) the returned MessageItem is the reader's unassembled default —
 * the reader's end state, not this function, distinguishes those outcomes.
 *
 * @tparam Delta  The reader's decoded event type.
 * @tparam Driver The request driver for THIS exchange (sse_request<Delta> or
 *                HttpRequestDriver<Delta>), deduced from the argument.
 * @param executor         Executor both sides run on: the connect here and the
 *                         spawned producer task.
 * @param model_endpoint   Where to connect; resolved as resolve_endpoint
 *                         parsed it from the ModelEndpoint's base_url.
 * @param request          The fully built provider request (an interpreter's
 *                         build_request product).
 * @param response_reader  The consumer half; also drives accumulation and
 *                         assembly on its consume() side.
 * @param driver           The one-exchange producer driver.
 * @return The reader's assembled model_io::MessageItem.
 * @throws HttpRequestException{Stage::Connect} when the connection cannot be
 *         established; whatever consume() surfaces for consumer-side faults
 *         (a throwing reader hook).
 */
template<
    typename Delta,
    RequestDriver<typename ModelResponseReader<Delta>::Handler> Driver
>
boost::asio::awaitable<model_io::MessageItem> complete_once(
    boost::asio::any_io_executor executor,
    ResolvedEndpoint model_endpoint,
    ModelRequestInterpreter::HttpRequest request,
    std::shared_ptr<ModelResponseReader<Delta>> response_reader,
    Driver driver
) {
    // ---- connect -----------------------------------------------------------
    // create_connection_stream throws boost::system::system_error exclusively
    // (DNS, TCP, timeout, certificate, SNI — model_request.hpp documents the
    // family). The system_error branch runs for all of those and keeps
    // e.code(): the category (netdb / asio.system / ssl / beast timeout) is
    // what callers classify connect failures by, and to_string() renders its
    // message. The wider branches are defensive depth only.
    connection_stream stream{};
    try {
        stream = co_await create_connection_stream(executor, model_endpoint);
    } catch (const boost::system::system_error& e) {
        throw HttpRequestException(
            HttpRequestException::Stage::Connect,
            e.what(), e.code(), {},
            model_endpoint.target,
            model_endpoint.host
        );
    } catch (const std::exception& e) {
        throw HttpRequestException(
            HttpRequestException::Stage::Connect,
            e.what(), {}, {},
            model_endpoint.target,
            model_endpoint.host
        );
    } catch (...) {
        throw HttpRequestException(
            HttpRequestException::Stage::Connect,
            "unknown error", {}, {},
            model_endpoint.target,
            model_endpoint.host
        );
    }

    // ---- producer: one spawned exchange, log-only failures ------------------
    // The canonical co_spawn wiring: pump() drives the driver over the stream
    // and finishes the handler on every exit path, so by the time an exception
    // reaches these catch blocks the consumer's get() below is already
    // unblocked and the outcome is routed through the consumer side. Letting
    // it escape a detached spawn would only terminate the process — so every
    // failure is rendered (with request context, via wrap_request_failure for
    // the non-http strays) into the log and nothing else.
    boost::asio::co_spawn(
        executor,
        [
            driver,
            reader_ = response_reader,
            stream_ = std::move(stream),
            request_ = std::move(request)
        ] () mutable -> boost::asio::awaitable<void> {
            try {
                // request_ passes as an lvalue, so pump() takes its own copy
                // and request_ stays readable in the fallback catches below.
                co_await reader_->pump(driver, std::move(stream_), request_);
            } catch (const HttpRequestException& error) {
                // Already stage- and context-rich: render as-is.
                logging::Logger::error(error.to_string());
            } catch (const std::exception& error) {
                // Defensive depth (the drivers wrap their own failures into
                // HttpRequestException): fold the stray into the module's
                // lifecycle exception so the log line keeps the request
                // context. Stage::Unknown — nothing more precise is known.
                auto http_exception = wrap_request_failure(
                    HttpRequestException::Stage::Unknown,
                    error.what(),
                    {},
                    request_
                );
                logging::Logger::error(http_exception.to_string());
            } catch (...) {
                auto http_exception = wrap_request_failure(
                    HttpRequestException::Stage::Unknown,
                    "unknown error",
                    {},
                    request_
                );
                logging::Logger::error(http_exception.to_string());
            }
        },
        boost::asio::detached
    );

    // ---- consumer: inline drain to the assembled item ------------------------
    // The lambda's reader_ capture shares ownership, so the spawned producer
    // cannot outlive the reader even if it is still tearing down when this
    // coroutine finishes first.
    auto model_response = co_await response_reader->consume();
    co_return model_response;
}

} // namespace endpoint
