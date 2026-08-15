#pragma once

#include <cstdlib>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <atomic>
#include <span>

#include <boost/none.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/core.hpp>
#include <boost/system.hpp>
#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include "endpoint/https_stream.hpp"
#include "endpoint/http_request_exception.hpp"

namespace endpoint {

/// Size of the fixed-size read buffer used to pull SSE body chunks off the
/// stream before handing them to the handler's put() side.
inline constexpr std::size_t DEFAULT_SSE_CHUNK_SIZE = 8192;

/// Default retention window for SSEResponseHandler: the number of already-
/// consumed lines kept behind the cursor so reset() can still rewind to a
/// recent checkpoint after rolling trim has dropped older history. Per-instance
/// configurable via the SSEResponseHandler constructor.
inline constexpr std::size_t DEFAULT_SSE_LINE_WINDOW = 1024;

template<typename Product>
class AsyncResponseHandler {
public:
    virtual ~AsyncResponseHandler() = default;

    virtual boost::asio::awaitable<void> put(std::string_view payload) = 0;
    virtual boost::asio::awaitable<Product> get() = 0;
};

// Lifecycle state of an SSEResponseHandler, hoisted to namespace scope so it
// can be carried by SSEAborted without making that exception a template.
enum class SSEHandlerState { RUNNING, RESUMABLE, ERROR, DONE };

// Streaming for diagnostics (Boost.Test assertions, logging).
inline std::ostream& operator<<(std::ostream& os, SSEHandlerState state) {
    switch (state) {
        case SSEHandlerState::RUNNING:   return os << "RUNNING";
        case SSEHandlerState::RESUMABLE: return os << "RESUMABLE";
        case SSEHandlerState::ERROR:     return os << "ERROR";
        case SSEHandlerState::DONE:      return os << "DONE";
    }
    return os << "SSEHandlerState(?)";
}

// Raised by SSEResponseHandler::put()/get() when an in-flight channel operation
// ends because the channel was closed — via finish(), or via put()'s own error
// path on a framing/decode fault — so the producer/consumer coroutine can break
// out. (suspend()/reset() do NOT close the channel and so never raise this.) The
// handler's state at throw time is captured and exposed via state(), so the
// catch site can distinguish ERROR from DONE directly from the exception,
// without re-querying the handler.
class SSEAborted : public std::runtime_error {
public:
    explicit SSEAborted(const char* what, SSEHandlerState state)
        : std::runtime_error(what), _state(state) {}

    SSEHandlerState state() const noexcept { return _state; }

private:
    SSEHandlerState _state;
};

/**
 * @brief Frames a Server-Sent Events byte stream into events and publishes them
 *        over an internal channel to a single consumer.
 *
 * Threading: this handler is only safe under a **single-reader / single-writer
 * (SPSC)** discipline. At most one producer task drives put() and at most one
 * consumer task drives get(); that single put↔get pair *may* run on separate
 * threads concurrently (that is what the underlying concurrent_channel and the
 * atomic state are for). No other concurrency is permitted:
 *   - never call put() from two tasks at once, nor get() from two at once;
 *   - never call suspend()/reset()/finish() concurrently with put()/get() or
 *     with each other — the line-buffer state (_buffer/_lines/_next_line/_base)
 *     they mutate is not synchronized.
 * In short: serialize everything except the lone put↔get pair.
 *
 * Typical roles:
 *   - Put side (producer): owns the https_stream, reads chunks and feeds put().
 *     On a dropped connection it calls suspend(RESUMABLE) to obtain a recommended
 *     restart index, reconnects (honouring SSE @c retry: / @c Last-Event-ID),
 *     then calls reset(index) and resumes put(). suspend()/reset() are put-side
 *     bookkeeping only — they mutate the line-buffer state but never touch the
 *     channel, so the get side is completely unaware of a reconnect and the
 *     queue stays readable. On an unrecoverable fault the put side may instead
 *     call finish(ERROR) to force-abort both ends.
 *   - Get side (consumer): drains get() and, when the stream is logically
 *     complete, calls finish(DONE). finish() is the *only* control that breaks a
 *     blocked get() out of its wait (it closes the channel).
 *
 * Memory: the consumed history is retained only up to a rolling window of
 * DEFAULT_SSE_LINE_WINDOW lines (configurable per instance), so a long-lived
 * stream such as a model delta feed keeps a bounded footprint instead of
 * accumulating every line ever seen. Lines keep absolute indices for their
 * lifetime — rolling trim advances _base rather than shifting the cursor, so
 * suspend()/reset() indices and _restart_index() overrides stay stable across
 * trims; a checkpoint older than the retained window has been trimmed away and
 * reset() rejects it with out_of_range. Trimming never invalidates a live span:
 * put() trims only after the last message span of a chunk has been consumed by
 * _handle_message().
 *
 * @tparam Product  The decoded event type. Subclasses override _handle_message()
 *                  to turn a framed event (a span of LineInfo) into a Product.
 */
template<typename Product>
class SSEResponseHandler: public AsyncResponseHandler<Product> {
public:
    using LineInfo = std::pair<std::string, std::string>;
    // The decoded event type this handler produces, named so wrapping
    // templates (e.g. PeekingHandler) can refer to it without knowing the
    // concrete Product parameter a handler was specialised with.
    using product_type = Product;
    // Lifecycle states. Aliased to the namespace-scope enum so call sites can
    // keep writing SSEResponseHandler<Product>::State while SSEAborted carries
    // the same type.
    using State = SSEHandlerState;

protected:
    // Tag stored in LineInfo::first to mark a blank line, which is the SSE
    // event delimiter. A real field line always carries a field name before
    // the colon, so this can never collide with a legitimate field name.
    static constexpr std::string_view BLANK_LINE = "empty";

    std::string _buffer;
    std::vector<LineInfo> _lines;
    std::size_t _next_line;
    // Absolute index of _lines[0]. Rolling trim erases the oldest consumed
    // lines from the front of _lines and advances _base by the same amount, so
    // _next_line (and any checkpoint a _restart_index() override caches) lives
    // in a stable coordinate space that trims never shift. reset() re-parks the
    // cursor without touching _base. Invariant:
    // _base <= _next_line <= _base + _lines.size().
    std::size_t _base;
    // Max number of consumed lines retained behind the cursor; see _trim().
    std::size_t _line_window;

    std::atomic<State> _state;
    // The leading error_code is the per-message status: clear for a normal
    // event, set when the producer reports a fault or closes the stream.
    boost::asio::experimental::concurrent_channel<void(boost::system::error_code, Product)> _queue;

    // Frame _buffer into complete (newline-terminated) lines. Each line becomes
    // a (field, value) entry; a blank line becomes (BLANK_LINE, ""). Lines
    // without a colon are comments/junk and are dropped.
    virtual void _split_line() {
        std::size_t endline_pos;
        while ((endline_pos = _buffer.find("\n")) != std::string::npos) {
            auto line = std::string_view(_buffer).substr(0, endline_pos);

            // Every read of `line` must happen before the erase below:
            // basic_string::erase invalidates any view into the string, so
            // touching `line` after it would read shifted-in bytes of the
            // following content.

            // Tolerate CRLF line endings by stripping a trailing '\r'.
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }

            if (line.empty()) {
                _lines.emplace_back(std::string(BLANK_LINE), "");
            } else {
                auto colon_pos = line.find(":");
                if (colon_pos != std::string_view::npos) {
                    auto field = line.substr(0, colon_pos);
                    std::size_t value_pos = colon_pos + 1;
                    // SSE skips exactly one leading space after the colon.
                    if (value_pos < line.size() && line[value_pos] == ' ') {
                        ++value_pos;
                    }
                    _lines.emplace_back(std::string(field), std::string(line.substr(value_pos)));
                }
                // else: comment / junk line (no colon) — drop.
            }

            // Consume the line plus its newline only once the view is dead.
            _buffer.erase(0, endline_pos + 1);
        }
        return;
    }

    // Return the next complete event: the run of lines up to a blank-line
    // delimiter. Leading blank lines are skipped, so the result is always
    // non-empty when a complete event exists. An empty span means no complete
    // event is available yet (the caller should wait for more input). _next_line
    // is an absolute line index; it is mapped into _lines coordinates via _base.
    virtual std::span<const LineInfo> _next_message() {
        std::size_t first = _next_line - _base;
        while (first < _lines.size() && _lines[first].first == BLANK_LINE) {
            ++first;
        }

        std::size_t last_line = first;
        while (last_line < _lines.size() && _lines[last_line].first != BLANK_LINE) {
            ++last_line;
        }

        if (last_line >= _lines.size()) {
            return {};
        }

        auto slice = std::span<const LineInfo>(_lines).subspan(first, last_line - first);
        _next_line = _base + last_line + 1;   // consume the delimiter
        return slice;
    }

    // Rolling trim: drop the oldest consumed lines once the retained history
    // exceeds _line_window, so a long-lived stream keeps a bounded footprint.
    // Only lines strictly before the cursor are eligible — _next_message()
    // hands out spans into [_next_line, ...), so touching anything at or after
    // the cursor would invalidate them. Call only where no message span is
    // alive: put() invokes this after its message loop, once the last span has
    // been consumed by _handle_message().
    void _trim() {
        if (_next_line - _base > _line_window) {
            std::size_t drop = (_next_line - _base) - _line_window;
            _lines.erase(
                _lines.begin(),
                _lines.begin() + static_cast<std::ptrdiff_t>(drop));
            _base += drop;
        }
    }

    // Decode one framed event into a Product. The default is a no-op;
    // subclasses override this to implement the SSE field handling. The span is
    // only valid for the duration of this call — do not retain it.
    virtual Product _handle_message(std::span<const LineInfo> /*message*/) {
        return Product{};
    }

    // Recommended line index to rewind to on reconnect, returned by suspend()
    // for the caller to hand to reset(). Indices are absolute line numbers in a
    // coordinate space that rolling trim never shifts (_base tracks the front
    // of _lines), so an override may cache a checkpoint and return it later.
    // The default is the current cursor (the last fully-delivered event
    // boundary), i.e. drop only the incomplete tail. Override to consult SSE
    // resume metadata — e.g. the line index of the last acknowledged `id:` (the
    // `retry:` field only governs reconnect timing, not position) — so reset()
    // can replay from the correct checkpoint. A checkpoint older than the
    // retained window has been trimmed away; reset() then throws out_of_range.
    virtual std::size_t _restart_index() const {
        return _next_line;
    }

public:
    explicit SSEResponseHandler(
        boost::asio::any_io_executor executor,
        std::size_t line_window = DEFAULT_SSE_LINE_WINDOW)
        : _buffer()
        , _lines()
        , _next_line(0)
        , _base(0)
        , _line_window(line_window)
        , _state(State::RUNNING)
        , _queue(executor) {}

    ~SSEResponseHandler() override = default;

    // std::atomic and concurrent_channel are neither copyable nor movable, so
    // these are deleted rather than left as misleading defaulted defaults.
    SSEResponseHandler(const SSEResponseHandler&) = delete;
    SSEResponseHandler& operator=(const SSEResponseHandler&) = delete;
    SSEResponseHandler(SSEResponseHandler&&) = delete;
    SSEResponseHandler& operator=(SSEResponseHandler&&) = delete;

    void set_state(State state) noexcept {
        _state.store(state, std::memory_order_release);
    }

    State get_state() const noexcept {
        return _state.load(std::memory_order_acquire);
    }

    // Feed a raw SSE chunk, frame it into events, and publish each completed
    // event. async_send suspends while the queue is full, giving natural
    // back-pressure against a slow consumer.
    //
    // Exception safety: _split_line()/_next_message()/_handle_message() allocate
    // and may throw (and _handle_message is user-overridable) — they are not
    // noexcept. Any such fault is fatal for the handler: put() drives it to ERROR
    // via finish() (which closes the channel and so also unblocks any waiting
    // get()), then rethrows so the producer's read loop can tear down. A channel
    // error reported through ec (finish() was called from elsewhere) becomes
    // SSEAborted carrying the current state.
    boost::asio::awaitable<void> put(std::string_view payload) override {
        try {
            _buffer.append(payload);
            _split_line();

            std::span<const LineInfo> message;
            while (!(message = _next_message()).empty()) {
                boost::system::error_code ec;
                co_await _queue.async_send(
                    boost::system::error_code{},
                    _handle_message(message),
                    boost::asio::redirect_error(boost::asio::use_awaitable, ec)
                );
                // Only close() surfaces here now (suspend()/reset() leave the
                // channel alone): the channel was closed by finish(), so the
                // state already holds the terminal value — propagate it.
                if (ec) {
                    throw SSEAborted("SSE put aborted", get_state());
                }
            }

            // Every message span handed to _handle_message above is dead by
            // now, so this is the only safe trim point — rolling trim must
            // never run while a span is (or might still be) alive.
            _trim();
        } catch (const SSEAborted&) {
            throw;
        } catch (...) {
            // Framing/decode fault: force the handler into ERROR and tear the
            // channel down (unblocking any waiting get()), then propagate the
            // original exception to the producer.
            finish(State::ERROR);
            throw;
        }
        co_return;
    }

    // Suspend until the next event is available. A non-clear ec means the
    // channel was closed — by finish() from the consumer, or by the producer's
    // put() error path — so get() throws SSEAborted carrying the handler's state
    // at that moment (ERROR or DONE). suspend()/reset() never cause this: they
    // do not touch the channel, so a blocked get() is unaffected by a put-side
    // reconnect.
    boost::asio::awaitable<Product> get() override {
        boost::system::error_code ec;
        Product product = co_await _queue.async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec) {
            throw SSEAborted("SSE get aborted", get_state());
        }
        co_return product;
    }

    // --- External control -------------------------------------------------

    // Put-side suspension: record the intended state (typically RESUMABLE) and
    // return the recommended restart index from _restart_index() — the line
    // checkpoint (an absolute line index) to hand to reset() after reconnecting.
    // This is pure put-side bookkeeping: it does NOT touch the channel, so the
    // get side is completely unaware and the queue stays readable (a blocked
    // get() keeps waiting normally, a draining get() keeps draining). The
    // producer is expected to call this when it is not itself blocked inside
    // put(); cancelling its own read coroutine is the producer's concern, not
    // the handler's.
    std::size_t suspend(State state) {
        std::size_t restart_at = _restart_index();   // capture before mutation
        set_state(state);
        return restart_at;
    }

    // Put-side rewind to a committed checkpoint `index` (an absolute line
    // index): keep the prefix up to it, drop the suffix, park _next_line at
    // `index` so only newly appended data is framed afterwards, clear the raw
    // buffer, and return the handler to RUNNING. Like suspend() this does NOT
    // touch the channel — the get side is unaware and the queue remains
    // readable. Not safe to call concurrently with put() (or
    // _split_line()/_next_message()): the line-buffer state they share is not
    // synchronized. The checkpoint must lie within the retained window — a
    // checkpoint below _base has already been dropped by rolling trim and is
    // rejected with out_of_range (the cursor itself, the default checkpoint, is
    // always within the window).
    void reset(std::size_t index) {
        if (index < _base || index > _base + _lines.size()) {
            throw std::out_of_range("SSEResponseHandler::reset: index out of range");
        }

        _buffer.clear();
        _lines.resize(index - _base);   // keep committed prefix, drop the suffix
        _next_line = index;             // cursor parked at the checkpoint
        set_state(State::RUNNING);
    }

    // Permanently end processing and forcibly abort any in-flight channel work
    // on BOTH sides. Records `state` then closes the channel: a blocked put()
    // and/or get() completes with channel_closed and throws SSEAborted carrying
    // this state, and further send/receive attempts fail the same way. This is
    // the only control that breaks a blocked get() out of its wait.
    //
    //   - get side, normal end:        finish() / finish(DONE);
    //   - put side, unrecoverable err: finish(ERROR) — force-aborts the consumer
    //     too, since no further events can be produced.
    void finish(State state = State::DONE) {
        set_state(state);
        _queue.close();
    }
};

/**
 * @brief Drive the put side of an SSEResponseHandler over a single TLS stream.
 *
 * Sends @p request, then pumps the streamed response body into @p handler->put()
 * chunk by chunk until the server closes the connection or the handler is
 * externally finished. The request is constructed by the caller (there is no
 * request-builder callback), and only the producer side is driven here — the
 * consumer drains get() elsewhere, on the same shared @p handler. Because
 * put() suspends while the channel is full, a slow consumer naturally
 * back-pressures this pump (and thus the network reads).
 *
 * One connection only. Reconnect/resume is the caller's concern: on a dropped
 * connection, catch the HttpRequestException, call handler->suspend(RESUMABLE)
 * to obtain a restart index, establish a new stream, handler->reset(index), and
 * call this again. This function never calls suspend()/reset()/finish(), nor
 * get(); the handler's lifecycle and the consumer side stay under the caller's
 * control. On return the handler is *not* finished — the caller decides that.
 *
 * A put() that throws SSEAborted (the channel was closed via finish() from
 * elsewhere) is treated as a cooperative stop: the pump ends the stream cleanly
 * and returns. Any other failure (bad HTTP status, network/SSL error, or a
 * framing/decode fault raised by the handler — which will already have driven it
 * to ERROR) surfaces as an HttpRequestException carrying the failing stage.
 *
 * @tparam Product  The handler's decoded event type.
 * @param handler   Shared with the consumer task; put() is the only method used.
 * @param stream    Single-use TLS stream; torn down on a clean end.
 * @param request   Fully constructed SSE request to send.
 */
template<typename Product>
boost::asio::awaitable<void> sse_request(
    std::shared_ptr<SSEResponseHandler<Product>> handler,
    std::unique_ptr<https_stream> stream,
    boost::beast::http::request<boost::beast::http::string_body> request)
{
    namespace http = boost::beast::http;

    if (!stream) {
        throw HttpRequestException(
            HttpRequestException::Stage::Unknown,
            "invalid stream pointer (= nullptr)");
    }
    if (!handler) {
        throw HttpRequestException(
            HttpRequestException::Stage::Unknown,
            "invalid handler pointer (= nullptr)");
    }

    // The request was built by the caller, so the first real stage is the write.
    HttpRequestException::Stage stage = HttpRequestException::Stage::Write;

    try {
        // Send the caller-constructed request on the existing TLS stream.
        boost::beast::get_lowest_layer(*stream).expires_after(
            std::chrono::seconds(DEFAULT_TIMEOUT_SEC));
        co_await http::async_write(*stream, request, boost::asio::use_awaitable);

        // Read only the header so the status can be checked before any event
        // bytes reach the handler. buffer_body delivers the body incrementally.
        stage = HttpRequestException::Stage::Read;
        boost::beast::flat_buffer buffer;
        http::response_parser<http::buffer_body> parser;
        // SSE streams have no bound on length; disable Beast's 8 MiB default.
        parser.body_limit(boost::none);
        co_await http::async_read_header(*stream, buffer, parser, boost::asio::use_awaitable);

        stage = HttpRequestException::Stage::HandleResponse;
        if (parser.get().result() != http::status::ok) {
            throw HttpRequestException(
                stage,
                "SSE request rejected with status " + std::to_string(parser.get().result_int()),
                {},
                std::string(request.method_string()),
                std::string(request.target()),
                std::string(request[http::field::host])
            );
        }

        // SSE connections sit idle between events, so the body phase must not be
        // aborted by the per-operation deadline set above.
        boost::beast::get_lowest_layer(*stream).expires_never();

        // Feed body bytes to the handler's put side. buffer_body writes directly
        // into `chunk`; `written` is how many bytes Beast produced this round.
        std::array<char, DEFAULT_SSE_CHUNK_SIZE> chunk;
        auto& body = parser.get().body();
        while (!parser.is_done()) {
            body.data = chunk.data();
            body.size = chunk.size();

            boost::system::error_code ec;
            co_await http::async_read_some(
                *stream,
                buffer,
                parser,
                boost::asio::redirect_error(boost::asio::use_awaitable, ec)
            );

            // buffer_body reports "output full, hand me another buffer" rather
            // than a genuine failure.
            if (ec == http::error::need_buffer) {
                ec = {};
            }

            const auto written = chunk.size() - body.size;
            if (written > 0) {
                // put() may throw SSEAborted when the channel was closed via
                // finish() from elsewhere — treat that as a cooperative stop and
                // end the pump cleanly. Any other throw (a framing/decode fault)
                // propagates; the handler has already driven itself to ERROR.
                try {
                    co_await handler->put(std::string_view{chunk.data(), written});
                } catch (const SSEAborted&) {
                    break;
                }
            }

            // An indefinite SSE stream ends when the peer closes the connection;
            // treat those as the clean end of the exchange.
            if (ec == http::error::partial_message ||
                ec == boost::asio::error::eof ||
                ec == boost::asio::ssl::error::stream_truncated) {
                break;
            }

            if (ec) {
                throw boost::system::system_error{ec};
            }
        }

        // Clean end (server closed) or cooperative stop: tear the stream down.
        boost::system::error_code shutdown_ec;
        co_await stream->async_shutdown(boost::asio::redirect_error(boost::asio::use_awaitable, shutdown_ec));
        // A peer may drop the connection before close_notify; the lowest-layer
        // close is best-effort (noexcept) to finish tearing the socket down.
        boost::beast::get_lowest_layer(*stream).close();
        co_return;
    }
    catch (const boost::system::system_error& exception) {
        throw HttpRequestException(
            stage,
            std::string("HTTP request failed: ") + exception.what(),
            exception.code(),
            std::string(request.method_string()),
            std::string(request.target()),
            std::string(request[http::field::host])
        );
    }
    catch (const HttpRequestException&) {
        // Preserve exceptions already enriched above (bad status, null args).
        throw;
    }
    catch (const std::exception& exception) {
        throw HttpRequestException(
            stage,
            std::string("HTTP request exception: ") + exception.what(),
            {},
            std::string(request.method_string()),
            std::string(request.target()),
            std::string(request[http::field::host])
        );
    }
}

}
