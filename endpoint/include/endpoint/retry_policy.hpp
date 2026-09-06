#pragma once

//
// retry_policy.hpp — the retry verdict + backoff shared by the two exchange
// engines (streaming `complete`, bounded `fetch`)
// ====================================================================
//
// Both whole-exchange engines in this module agree on what "recoverable"
// means and how long to wait between attempts. That agreement is the
// retry_policy base class: the rolling backoff state (backoff, timer) plus the
// default recoverability verdict and the default exponential backoff step.
//
// The engines differ only in what one ATTEMPT is — complete_once spawns a
// streaming producer/consumer pair, fetch_once runs one bounded whole-body
// exchange — and in what a failed attempt reports back (the reader's end
// state vs. the HttpRequestException itself). The policy is deliberately a
// non-template base so the verdict table and the backoff arithmetic each live
// in exactly one place; subclass to override _recoverable/_sleep per
// deployment (providers disagree at the margins).

#include <algorithm>
#include <chrono>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/beast/core/error.hpp>

#include "endpoint/http_request_exception.hpp"

namespace endpoint {

class retry_policy {
protected:
    boost::asio::any_io_executor _executor;
    const std::chrono::milliseconds _initial_backoff;
    const std::chrono::milliseconds _max_backoff;
    const unsigned int _max_retry_attempts;
    std::chrono::milliseconds _backoff;
    boost::asio::steady_timer _timer;

    /**
     * @brief One backoff step: wait out the CURRENT _backoff, then advance
     *        it (double, capped at _max_backoff) for the next attempt.
     *
     * The wait comes FIRST so the first retry sleeps exactly _initial_backoff
     * (not 2× it) and the sequence is initial, 2×initial, 4×initial, ….
     * Called between attempts only — the budget's last failure throws
     * instead of sleeping, so the advance is never used past the last wait.
     * Override to inject jitter or to honour a provider's Retry-After
     * semantics; the timer wait swallows its error_code (a cancelled wait
     * proceeds to the retry immediately).
     */
    virtual boost::asio::awaitable<void> _sleep() {
        boost::system::error_code ec;
        _timer.expires_after(_backoff);
        co_await _timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (_backoff <= (_max_backoff / 2u)) {
            _backoff *= 2u;
        } else {
            _backoff = _max_backoff;
        }
        co_return;
    }

    /**
     * @brief The default recoverability verdict for one failed attempt.
     *
     * A recoverable failure is one a FRESH attempt could plausibly fix:
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
     * the engine and replace this verdict).
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
     * @brief Construct the retry policy for one executor.
     *
     * @param executor            Executor every attempt and backoff runs on;
     *                            bound here, so the object outlives every
     *                            call made on it.
     * @param initial_backoff     Backoff before the FIRST retry; doubles
     *                            per retry up to max_backoff. No jitter by
     *                            default (deterministic tests); thread
     *                            randomness through a _sleep override if
     *                            fleet-level thundering herds matter.
     * @param max_backoff         Backoff ceiling; clamped up to
     *                            initial_backoff when smaller.
     * @param max_retry_attempts  The MAXIMUM number of RETRIES after the
     *                            initial attempt — only retries count (the
     *                            initial attempt does not, so the count
     *                            starts at 0), and a successful attempt
     *                            returns immediately. 0 means no retry at
     *                            all: one attempt, whose failure propagates
     *                            immediately.
     */
    retry_policy(
        boost::asio::any_io_executor executor,
        std::chrono::milliseconds initial_backoff = std::chrono::milliseconds{500},
        std::chrono::milliseconds max_backoff = std::chrono::milliseconds{120000},
        unsigned int max_retry_attempts = 3)
        : _executor(std::move(executor)),
          _initial_backoff(initial_backoff),
          _max_backoff(max_backoff > initial_backoff ? max_backoff : initial_backoff),
          _max_retry_attempts(std::max(max_retry_attempts, 0u)),
          _backoff(_initial_backoff),
          _timer(_executor) {}

    // A timer bound to the executor plus rolling retry state: copying is
    // impossible (steady_timer is not copyable) and moving would strand a
    // pending wait on a gutted object, so both are deleted rather than left
    // as misleading defaults. Subclassing — the _recoverable/_sleep extension
    // points — is the intended reuse.
    virtual ~retry_policy() = default;
    retry_policy(const retry_policy&) = delete;
    retry_policy& operator=(const retry_policy&) = delete;
    retry_policy(retry_policy&&) = delete;
    retry_policy& operator=(retry_policy&&) = delete;
};

} // namespace endpoint
