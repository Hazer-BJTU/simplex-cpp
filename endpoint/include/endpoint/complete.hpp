#pragma once

//
// complete.hpp — one model invocation, end to end: connect, exchange, assemble
// ====================================================================
//
// The whole-exchange conveniences over the canonical wiring (see the
// model_request.hpp header block). complete_once connects to the resolved
// endpoint, runs ONE driver exchange as a spawned producer task, drains the
// reader's consumer side inline, and hands back the reader's assembled
// model_io::MessageItem. The complete functor wraps complete_once with a
// plain retry loop: on a recoverable failure the WHOLE exchange is discarded
// and re-read from scratch (fresh connect, the reader reset via clear()) —
// most providers offer no stream resume, so a full re-read is the only
// resumption there is.
//
// Failure handling inside complete_once splits by side:
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

#include <algorithm>
#include <chrono>
#include <concepts>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/core/error.hpp>

#include "endpoint/http_request_exception.hpp"
#include "endpoint/model_request.hpp"

#include "logging/logger.hpp"

namespace endpoint {

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

/**
 * @brief complete_once with plain retry, as a callable object: on a
 *        recoverable failure, reset the reader and re-read the WHOLE
 *        response from scratch.
 *
 * A stateful retry engine bound to one executor — construct it once and call
 * it per exchange, exactly like a driver:
 *
 *     endpoint::complete complete_model{io.get_executor()};
 *     auto response = co_await complete_model(
 *         resolved_endpoint,
 *         interpreter->build_request(state, endpoint, generation),
 *         reader,
 *         endpoint::sse_request<my::Delta>);
 *
 * One call is up to _max_retry_attempts + 1 complete_once exchanges: the
 * INITIAL exchange plus, while failures classify as recoverable, one retry
 * each (the initial exchange is NOT counted against the retry budget). Every
 * attempt is a fresh connect, the SAME @p request re-sent verbatim, and the
 * SAME reader reset to its just-constructed state via clear() — accumulators,
 * end-state flags, and the handler's framing buffer and channel all rewind,
 * so no per-attempt factory is needed. Request building deliberately lives
 * OUTSIDE this layer (the provider wrapper above builds whatever it needs —
 * timestamps, idempotency keys — and owns freshness across retries); this
 * layer only re-sends.
 *
 * Hooks registered on the reader SURVIVE clear() and observe every attempt's
 * deltas (a monitor pre-wires once; note it when counting). Drivers are
 * reused across attempts too — keep them stateless, as the stock ones are.
 *
 * An attempt SUCCEEDS only on the reader's Completed end state; every other
 * outcome is classified for retry:
 *
 *   * end_error() set — the producer-side exception complete_once already
 *     logged; rethrown here and classified by _recoverable (a non-recoverable
 *     one propagates immediately, no further attempts).
 *   * Faulted with no end_error() — the stream ended truncated (server
 *     close without the terminal event): recoverable by design, retried to
 *     the budget, then reported as HttpRequestException{Read}.
 *   * Aborted — the consumer called reader->abort() (a deadline firing, a
 *     caller backing out): NOT retried, however much budget remains;
 *     surfaces as HttpRequestException{Unknown} with the request context.
 *
 * Connect failures propagate out of complete_once directly and classify the
 * same way. A hook/_accumulate fault inside consume() is a non-HTTP
 * exception: the caller's own bug — logged, then surfaced without retry and
 * without wrapping.
 *
 * The final failure propagates as-is (the last attempt's exception, or the
 * truncation/abort wrapping above); the give-up is recorded in the log
 * alongside complete_once's own per-exchange renderings.
 *
 * Concurrency: ONE operator() in flight per instance — the retry state
 * (_backoff, _timer) is shared and unsynchronized. Run concurrent exchanges
 * on separate instances (they share nothing but the executor).
 */
class complete {
protected:
    boost::asio::any_io_executor _executor;
    const std::chrono::milliseconds _initial_backoff;
    const std::chrono::milliseconds _max_backoff;
    const unsigned int _max_retry_attempts;
    std::chrono::milliseconds _backoff;
    boost::asio::steady_timer _timer;

    /**
     * @brief One backoff step: double _backoff (capped at _max_backoff),
     *        then wait it out.
     *
     * Called between attempts only — the budget's last failure throws
     * instead of sleeping, so _backoff never doubles past its use. Override
     * to inject jitter or to honour a provider's Retry-After semantics; the
     * timer wait swallows its error_code (a cancelled wait proceeds to the
     * retry immediately).
     */
    virtual boost::asio::awaitable<void> _sleep() {
        if (_backoff <= (_max_backoff / 2u)) {
            _backoff *= 2u;
        } else {
            _backoff = _max_backoff;
        }

        boost::system::error_code ec;
        _timer.expires_after(_backoff);
        co_await _timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
        co_return;
    }

    /**
     * @brief The default recoverability verdict for one failed exchange.
     *
     * A recoverable failure is one a FRESH exchange could plausibly fix:
     * server overload, a transient network state, a truncated stream. The
     * verdict is made on the HttpRequestException's own fields — status
     * first (the provider's answer, when there is one), then the
     * stage/error-code pair:
     *
     *   * HTTP status 429 / 408 / 5xx — recoverable (back off and retry).
     *   * any other status (401, 403, 400, 404, 422, ...) — NOT: the same
     *     request will draw the same answer; retrying only re-bills tokens.
     *   * Connect failures classify by error-code category: DNS try-again,
     *     refused / reset / timed-out, the stream-level connect timeout, and
     *     a truncated TLS handshake are transient; authoritative
     *     host-not-found and certificate verification failures are not
     *     (retrying cannot fix either).
     *   * Write/Read mid-exchange (a dropped connection, a read timeout —
     *     the bounded driver's HttpRequestTimeoutException included) —
     *     recoverable: the request itself was sound.
     *   * HandleResponse with no status — recoverable: a decode fault the
     *     retry budget absorbs (a genuinely changed wire protocol exhausts
     *     it fast).
     *   * CreateRequest / Unknown — NOT: our own request-building bug, or
     *     something unwrapped and unclassifiable; fail fast.
     *
     * Providers disagree at the margins; override per deployment (subclass
     * complete and replace this verdict).
     */
    virtual bool _recoverable(const HttpRequestException& failure) noexcept {
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

public:
    /**
     * @brief Construct the retry engine for one executor.
     *
     * @param executor            Executor every exchange and backoff runs
     *                            on; bound here, so the object outlives
     *                            every call made on it.
     * @param initial_backoff     Backoff before the FIRST retry; doubles
     *                            per retry up to max_backoff. No jitter by
     *                            default (deterministic tests); thread
     *                            randomness through a _sleep override if
     *                            fleet-level thundering herds matter.
     * @param max_backoff         Backoff ceiling; clamped up to
     *                            initial_backoff when smaller.
     * @param max_retry_attempts  RETRIES after the initial exchange — the
     *                            initial exchange is not counted. A retry
     *                            re-sends the request and re-bills its
     *                            tokens, so keep this small; 0 is clamped
     *                            to 1.
     */
    complete(
        boost::asio::any_io_executor executor,
        std::chrono::milliseconds initial_backoff = std::chrono::milliseconds{500},
        std::chrono::milliseconds max_backoff = std::chrono::milliseconds{120000},
        unsigned int max_retry_attempts = 3
    ): _executor(std::move(executor)),
       _initial_backoff(initial_backoff),
       _max_backoff(max_backoff > initial_backoff ? max_backoff : initial_backoff),
       _max_retry_attempts(std::max(max_retry_attempts, 1u)),
       _backoff(_initial_backoff),
       _timer(_executor) {}

    // A timer bound to the executor plus rolling retry state: copying is
    // impossible (steady_timer is not copyable) and moving would strand a
    // pending wait on a gutted object, so both are deleted rather than left
    // as misleading defaults. Subclassing — the _recoverable/_sleep extension
    // points — is the intended reuse.
    virtual ~complete() = default;
    complete(const complete&) = delete;
    complete& operator = (const complete&) = delete;
    complete(complete&&) = delete;
    complete& operator = (complete&&) = delete;

    /**
     * @brief Run one whole exchange with plain retry.
     *
     * See the class doc for the retry semantics, the classification outcomes
     * and the concurrency contract; the parameters are one attempt's wiring,
     * reused verbatim every attempt.
     *
     * @tparam Delta         The reader's decoded event type — usually named
     *                       explicitly at the call site (a shared_ptr to a
     *                       READER SUBCLASS cannot deduce it).
     * @tparam Driver        The request driver per exchange; deduced, must
     *                       be copyable (reused across attempts).
     * @param model_endpoint Where to connect, every attempt.
     * @param request        The fully built provider request, re-sent
     *                       verbatim every attempt; building it is the
     *                       caller's (the provider layer's) concern.
     * @param reader         The consumer half, REUSED across attempts:
     *                       clear() rewinds it (and its handler) before
     *                       every attempt, so hand in a fresh one or a
     *                       previously drained one.
     * @param driver         The exchange driver, every attempt.
     * @return The assembled model_io::MessageItem of the first attempt that
     *         reached the terminal event.
     */
    template<typename Delta, RequestDriver<typename ModelResponseReader<Delta>::Handler> Driver>
    boost::asio::awaitable<model_io::MessageItem> operator () (
        ResolvedEndpoint model_endpoint,
        ModelRequestInterpreter::HttpRequest request,
        std::shared_ptr<ModelResponseReader<Delta>> reader,
        Driver driver
    ) {
        // Attempt 0 is the initial exchange; 1.._max_retry_attempts are the
        // retries — the initial request is NOT counted against the budget.
        for (unsigned int attempt = 0; attempt <= _max_retry_attempts; ++attempt) {
            // The reuse reset — before EVERY attempt (the first included), so
            // a handed-in reader's prior life cannot leak into attempt 0: the
            // end-state flags, the post-mortem exception, the accumulators,
            // and the handler's framing buffer + channel (rebuilt) all
            // rewind. The producer of the previous attempt has exited by
            // here (complete_once returned), and its spawned wrapper touches
            // nothing shared after pump's finish, so the reset cannot race
            // it.
            reader->clear();
            bool self_aborted = false;
            try {
                auto item = co_await complete_once<Delta>(_executor, model_endpoint, request, reader, driver);

                const auto end = reader->end_state();
                if (end == ModelResponseReader<Delta>::EndState::Completed) {
                    co_return item;
                }

                if (end == ModelResponseReader<Delta>::EndState::Aborted) {
                    // The consumer asked out; never turn that into a retry —
                    // the flag below bypasses classification entirely, so
                    // even a permissive _recoverable cannot outvote the
                    // caller.
                    self_aborted = true;
                    throw wrap_request_failure(
                        HttpRequestException::Stage::Unknown,
                        "stream aborted by the consumer before the terminal event",
                        {}, request
                    );
                }

                if (reader->end_error()) {
                    // complete_once already rendered it in the log; surface
                    // it for classification below.
                    std::rethrow_exception(reader->end_error());
                }

                // Faulted with no producer exception: truncated without the
                // terminal event — the case plain retry exists for.
                throw wrap_request_failure(
                    HttpRequestException::Stage::Read,
                    "stream ended without the terminal event",
                    {}, request
                );
            } catch (const HttpRequestException& failure) {
                if (self_aborted || !_recoverable(failure) || attempt == _max_retry_attempts) {
                    // The verdict is final and the failure — the report —
                    // propagates untouched; this line is the retry layer's
                    // own record of giving up, with the budget spent
                    // (each exchange's own failure was already logged by
                    // complete_once's producer spawn).
                    logging::Logger::error(
                        "exchange failed, giving up after "
                        + std::to_string(attempt + 1) + " of "
                        + std::to_string(_max_retry_attempts + 1)
                        + " attempts: " + failure.to_string());
                    throw;
                }
                // Recoverable, budget remains: one line before the backoff
                // so the retry is visible in the log too (the exchange's
                // own failure rendering came from complete_once).
                logging::Logger::info(
                    "transient failure, exchange attempt "
                    + std::to_string(attempt + 1) + " of "
                    + std::to_string(_max_retry_attempts + 1)
                    + " failed, retrying: " + failure.to_string());
            } catch (const std::exception& failure) {
                // A non-HTTP exception out of complete_once is
                // consumer-side (a hook or _accumulate fault out of
                // consume()) — the caller's own bug, never retry material:
                // rendered for the log, then surfaced as-is.
                logging::Logger::error(
                    std::string("exchange failed with a non-HTTP exception, "
                                "not retrying: ") + failure.what());
                throw;
            } catch (...) {
                logging::Logger::error(
                    "exchange failed with an unknown exception, not retrying");
                throw;
            }

            co_await _sleep();
        }

        // Unreachable: every path out of the last iteration returns or
        // throws.
        throw std::logic_error("endpoint::complete: retry loop fell through");
    }
};

} // namespace endpoint
