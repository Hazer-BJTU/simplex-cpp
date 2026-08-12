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

#include <nlohmann/json.hpp>

#include "endpoint/https_stream.hpp"
#include "exceptions/http_request_exception.hpp"

namespace endpoint {

/// Deadline applied to each stage (write, read) of a single JSON request/response
/// cycle. It is deliberately longer than the connect/handshake deadline
/// (DEFAULT_TIMEOUT_SEC) so that legitimately slow services are not prematurely
/// treated as unreachable.
inline constexpr std::size_t JSON_REQUEST_TIMEOUT_SEC = 60;

template<typename RequestHandler, typename ResponseHandler>
boost::asio::awaitable<nlohmann::json> json_request_once(
    std::unique_ptr<https_stream>& stream,
    const nlohmann::json& json_payload,
    RequestHandler&& request_handler,
    ResponseHandler&& response_handler
)
{
    namespace http = boost::beast::http;

    if (!stream) {
        throw HttpRequestException(
            HttpRequestException::Stage::Unknown,
            "invalid stream pointer (= nullptr)");
    }

    http::request<http::string_body> request;
    // Track the active operation so one catch block can attach the precise
    // failure stage while keeping the request flow easy to read.
    HttpRequestException::Stage stage =
        HttpRequestException::Stage::CreateRequest;

    try {
        // Convert the JSON payload into the caller-specific HTTP request.
        request = request_handler(json_payload);

        // Send the fully constructed request on the existing TLS stream.
        stage = HttpRequestException::Stage::Write;
        // Give this stage its own deadline. The deadline is longer than the
        // connect/handshake one to tolerate genuinely slow services; if it
        // fires Beast aborts with beast::error::timeout, which the catch
        // below reports against this stage.
        boost::beast::get_lowest_layer(*stream).expires_after(
            std::chrono::seconds(JSON_REQUEST_TIMEOUT_SEC));
        co_await http::async_write(
            *stream,
            request,
            boost::asio::use_awaitable);

        // Read one complete HTTP response. The buffer only needs to live for
        // this operation because Beast transfers the body into response.
        stage = HttpRequestException::Stage::Read;
        // Reset the deadline so a slow write cannot eat into the read window.
        boost::beast::get_lowest_layer(*stream).expires_after(
            std::chrono::seconds(JSON_REQUEST_TIMEOUT_SEC));
        boost::beast::flat_buffer buffer;
        http::response<http::dynamic_body> response;
        co_await http::async_read(
            *stream,
            buffer,
            response,
            boost::asio::use_awaitable);

        // Let the caller validate and decode the service-specific response.
        stage = HttpRequestException::Stage::HandleResponse;
        if (response.result() != http::status::ok) {
            throw HttpRequestException(
                stage,
                std::format("HTTP request returns code: {}", response.result_int()),
                {},
                std::string(request.method_string()),
                std::string(request.target()),
                std::string(request[http::field::host]));
        }

        auto json_response = response_handler(std::move(response));
        co_return json_response;
    }
    catch (const boost::system::system_error& exception) {
        throw HttpRequestException(
            stage,
            std::string("HTTP request failed: ") + exception.what(),
            exception.code(),
            std::string(request.method_string()),
            std::string(request.target()),
            std::string(request[http::field::host]));
    }
    catch (const HttpRequestException&) {
        // Preserve exceptions already enriched by a request/response handler.
        throw;
    }
    catch (const std::exception& exception) {
        throw HttpRequestException(
            stage,
            std::string("HTTP request exception: ") + exception.what(),
            {},
            std::string(request.method_string()),
            std::string(request.target()),
            std::string(request[http::field::host]));
    }
}

inline constexpr size_t DEFAULT_SSE_CHUNK_SIZE = 8192;

/**
 * @brief Perform a one-shot Server-Sent Events (SSE) request over a TLS stream.
 *
 * Unlike @ref json_request_once, an SSE exchange is streamed: once the response
 * headers arrive the body is read incrementally and every chunk is forwarded to
 * @p sse_parser as a @c std::string_view. The parser owns the event framing and
 * any accumulated state — this function only pumps bytes, so its concrete
 * implementation is supplied by the caller and left out here.
 *
 * SSE connections are single-use. On a successful exchange the stream is
 * explicitly shut down and @p stream is reset to @c nullptr so it cannot be
 * reused. On any failure the stream is left untouched for the caller to inspect
 * or discard.
 *
 * @tparam RequestHandler Callable `http::request<http::string_body>(const nlohmann::json&)`.
 * @tparam SSEParser      Callable `void(std::string_view)` invoked once per body chunk.
 */
template<typename RequestHandler, typename SSEParser>
boost::asio::awaitable<void> json_request_once_sse(
    std::unique_ptr<https_stream> stream,
    const nlohmann::json& json_payload,
    RequestHandler&& request_handler,
    SSEParser&& sse_parser
)
{
    namespace http = boost::beast::http;

    if (!stream) {
        throw HttpRequestException(
            HttpRequestException::Stage::Unknown,
            "invalid stream pointer (= nullptr)");
    }

    http::request<http::string_body> request;
    // Track the active operation so one catch block can attach the precise
    // failure stage while keeping the request flow easy to read.
    HttpRequestException::Stage stage =
        HttpRequestException::Stage::CreateRequest;

    try {
        // Convert the JSON payload into the caller-specific HTTP request.
        request = request_handler(json_payload);

        // Bound the request + header exchange; the body phase is unbounded.
        stage = HttpRequestException::Stage::Write;
        boost::beast::get_lowest_layer(*stream).expires_after(
            std::chrono::seconds(DEFAULT_TIMEOUT_SEC));
        co_await http::async_write(
            *stream,
            request,
            boost::asio::use_awaitable);

        // Read only the response header so the status can be validated before
        // any event bytes reach the parser. buffer_body lets the body be
        // delivered incrementally below instead of buffered in full.
        stage = HttpRequestException::Stage::Read;
        boost::beast::flat_buffer buffer;
        http::response_parser<http::buffer_body> parser;
        // SSE streams have no bound on length; disable Beast's 8 MiB default.
        parser.body_limit(boost::none);
        co_await http::async_read_header(
            *stream,
            buffer,
            parser,
            boost::asio::use_awaitable);

        stage = HttpRequestException::Stage::HandleResponse;
        if (parser.get().result() != http::status::ok) {
            throw HttpRequestException(
                stage,
                "SSE request rejected with status " +
                    std::to_string(parser.get().result_int()),
                {},
                std::string(request.method_string()),
                std::string(request.target()),
                std::string(request[http::field::host]));
        }

        // SSE connections sit idle between events, so the body phase must not
        // be aborted by the per-operation deadline set above.
        boost::beast::get_lowest_layer(*stream).expires_never();

        // Feed body bytes to the parser until the server ends the stream.
        // buffer_body writes directly into `chunk`; `written` is how many bytes
        // Beast produced in this round.
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
                boost::asio::redirect_error(
                    boost::asio::use_awaitable, ec));

            // buffer_body reports "output full, hand me another buffer" rather
            // than a genuine failure.
            if (ec == http::error::need_buffer)
                ec = {};

            const auto written = chunk.size() - body.size;
            if (written > 0)
                sse_parser(std::string_view{chunk.data(), written});

            // An indefinite SSE stream ends when the peer closes the
            // connection; treat those as the clean end of the exchange.
            if (ec == http::error::partial_message ||
                ec == boost::asio::error::eof ||
                ec == boost::asio::ssl::error::stream_truncated)
                break;
            if (ec)
                throw boost::system::system_error{ec};
        }

        // Success: the connection was consumed for this single SSE exchange.
        // Tear it down explicitly and release the caller's ownership so the
        // stream cannot be reused.
        boost::system::error_code shutdown_ec;
        co_await stream->async_shutdown(boost::asio::redirect_error(
            boost::asio::use_awaitable, shutdown_ec));
        // A peer may drop the connection before close_notify; the lowest-layer
        // close is best-effort (noexcept) to finish tearing the socket down.
        boost::beast::get_lowest_layer(*stream).close();
        stream.reset();
        co_return;
    }
    catch (const boost::system::system_error& exception) {
        throw HttpRequestException(
            stage,
            std::string("HTTP request failed: ") + exception.what(),
            exception.code(),
            std::string(request.method_string()),
            std::string(request.target()),
            std::string(request[http::field::host]));
    }
    catch (const HttpRequestException&) {
        // Preserve exceptions already enriched above (bad status, null stream).
        throw;
    }
    catch (const std::exception& exception) {
        throw HttpRequestException(
            stage,
            std::string("HTTP request exception: ") + exception.what(),
            {},
            std::string(request.method_string()),
            std::string(request.target()),
            std::string(request[http::field::host]));
    }
}

template<typename Product>
class AsyncResponseHandler {
public:
    virtual ~AsyncResponseHandler() = default;

    virtual boost::asio::awaitable<void> put(std::string_view payload) = 0;
    virtual boost::asio::awaitable<Product> get() = 0;
};

// Raised by SSEResponseHandler::put()/get() when an in-flight channel operation
// is aborted by an external control call — suspend(), finish(), or reset() on a
// closed channel — so the producer/consumer coroutine can break out and inspect
// the handler's state. Catch this and call get_state() to distinguish RESUMABLE
// (suspended) from DONE (finished).
class SSEAborted : public std::runtime_error {
public:
    explicit SSEAborted(const char* what) : std::runtime_error(what) {}
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
 *     with each other — the line-buffer state (_buffer/_lines/_next_line) they
 *     mutate is not synchronized.
 * In short: serialize everything except the lone put↔get pair.
 *
 * Typical roles:
 *   - Put side (producer): owns the https_stream, reads chunks and feeds put().
 *     On a dropped connection it calls suspend() to obtain a recommended restart
 *     index, reconnects (honouring SSE @c retry: / @c Last-Event-ID), then calls
 *     reset(index) and resumes put(). suspend()/reset() are therefore normally
 *     driven from the put side.
 *   - Get side (consumer): drains get() and, when the stream is logically
 *     complete, calls finish() to drive the handler to DONE.
 *
 * @tparam Product  The decoded event type. Subclasses override _handle_message()
 *                  to turn a framed event (a span of LineInfo) into a Product.
 */
template<typename Product>
class SSEResponseHandler: public AsyncResponseHandler<Product> {
public:
    using LineInfo = std::pair<std::string, std::string>;
    enum class State { RUNNING, RESUMABLE, ERROR, DONE };

protected:
    // Tag stored in LineInfo::first to mark a blank line, which is the SSE
    // event delimiter. A real field line always carries a field name before
    // the colon, so this can never collide with a legitimate field name.
    static constexpr std::string_view BLANK_LINE = "empty";

    std::string _buffer;
    std::vector<LineInfo> _lines;
    std::size_t _next_line;

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
    // event is available yet (the caller should wait for more input).
    virtual std::span<const LineInfo> _next_message() {
        while(_next_line < _lines.size() && _lines[_next_line].first == BLANK_LINE) {
            ++_next_line;
        }

        std::size_t last_line = _next_line;
        while (last_line < _lines.size() && _lines[last_line].first != BLANK_LINE) {
            ++last_line;
        }

        if (last_line >= _lines.size()) {
            return {};
        }

        auto slice = std::span<const LineInfo>(_lines).subspan(_next_line, last_line - _next_line);
        _next_line = last_line + 1;   // consume the delimiter
        return slice;
    }

    // Decode one framed event into a Product. The default is a no-op;
    // subclasses override this to implement the SSE field handling.
    virtual Product _handle_message(std::span<const LineInfo> /*message*/) {
        return Product{};
    }

    // Recommended line index to rewind to on reconnect, returned by suspend()
    // for the caller to hand to reset(). The default is the current cursor
    // (the last fully-delivered event boundary), i.e. drop only the incomplete
    // tail. Override to consult SSE resume metadata — e.g. the line index of the
    // last acknowledged `id:` (the `retry:` field only governs reconnect timing,
    // not position) — so reset() can replay from the correct checkpoint.
    virtual std::size_t _restart_index() const {
        return _next_line;
    }

public:
    explicit SSEResponseHandler(boost::asio::any_io_executor executor)
        : _buffer()
        , _lines()
        , _next_line(0)
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
    // back-pressure against a slow consumer. Throws SSEAborted if the channel
    // operation is aborted by suspend()/finish()/reset().
    boost::asio::awaitable<void> put(std::string_view payload) override {
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
            // cancel()/close() surfaces here as a non-clear ec; convert it so
            // the producer's read loop can break out and check the state.
            if (ec) {
                throw SSEAborted("SSE put aborted");
            }
        }
        co_return;
    }

    // Suspend until the next event is available. A non-clear ec means the
    // channel operation was aborted by suspend()/finish() (or the producer
    // reported a fault); _state then holds the precise state for the caller.
    boost::asio::awaitable<Product> get() override {
        boost::system::error_code ec;
        Product product = co_await _queue.async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, ec));

        if (ec) {
            throw SSEAborted("SSE get aborted");
        }
        co_return product;
    }

    // --- External control -------------------------------------------------

    // Manually suspend processing from another context: mark the handler
    // resumable and cancel any channel operation currently blocked inside
    // put()/get() (they complete with channel_cancelled and throw SSEAborted).
    // Returns the recommended restart index (from _restart_index()) — the line
    // checkpoint to pass to reset() after reconnecting. The channel stays open;
    // reset() reopens the slate for resumption.
    std::size_t suspend() {
        std::size_t restart_at = _restart_index();   // capture before mutation
        set_state(State::RESUMABLE);
        _queue.cancel();
        return restart_at;
    }

    // Rewind to a committed checkpoint `index` into _lines. Keeps the committed
    // prefix _lines[0, index), drops the tail, parks _next_line at `index` so
    // that only newly appended data is delivered, clears the raw buffer and the
    // channel, and returns the handler to RUNNING.
    //
    // Not safe to call concurrently with put()/get()/_split_line(): call it
    // only after the producer and consumer have observed the suspended state
    // (via suspend()) and stopped touching the handler.
    void reset(std::size_t index) {
        if (index > _lines.size()) {
            throw std::out_of_range("SSEResponseHandler::reset: index out of range");
        }

        _buffer.clear();
        _lines.resize(index);   // keep committed prefix, drop the suffix
        _next_line = index;     // cursor parked at the checkpoint
        _queue.reset();         // drop buffered products and reopen the channel
        set_state(State::RUNNING);
    }

    // Permanently end processing: mark the handler done and close the channel.
    // A blocked put() completes with channel_closed and throws SSEAborted;
    // further send/receive attempts fail the same way.
    void finish() {
        set_state(State::DONE);
        _queue.close();
    }
};

}
