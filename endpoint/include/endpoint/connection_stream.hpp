#pragma once

//
// connection_stream.hpp — the runtime-flavour connection facade
// =============================================================
//
// The endpoint module's transport used to be parameterised on its stream
// flavour at compile time (sse_request<Product, Stream>, http_request<...>,
// RequestDriver<..., Stream>): every layer above the socket carried a Stream
// template parameter purely so the bottom layer could pick beast's TLS or
// plain calls — even though the flavour is a RUNTIME fact (the base_url's
// scheme) that a template parameter can only fake with two instantiations of
// everything.
//
// connection_stream closes that gap. It is a move-only value owning exactly
// one live connection of either flavour behind one type:
//
//   * constructed empty (no connection) or by adopting a connected
//     https_stream / http_stream (a null pointer degenerates to empty);
//   * every operation dispatches to the held flavour at runtime — the
//     deadline controls, the HTTP reads/writes, the teardown;
//   * an operation on an EMPTY stream reports the bug the way the drivers
//     used to report a null pointer: HttpRequestException{Stage::Unknown}.
//
// The canonical producer is create_connection_stream(executor, resolved)
// (model_request.hpp), which resolves the endpoint's scheme at runtime and
// returns the connected facade by value. The flavour-specific factories in
// https_stream.hpp are deprecated.
//
// Ownership discipline is unchanged from the underlying flavours: exactly one
// owner per live connection; move the value to move the connection; never
// shared across threads.
//

#include <chrono>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include "endpoint/http_request_exception.hpp"
#include "endpoint/https_stream.hpp"

namespace endpoint {

/**
 * @brief One live connection of either flavour, behind a move-only value.
 *
 * Holds a connected https_stream (TLS) or http_stream (plain TCP) and
 * dispatches each operation to the held flavour at runtime. Callers stay
 * flavour-agnostic: no template parameter, no std::visit at the call site.
 *
 * An empty stream (default-constructed, moved-from adoption of a null
 * pointer, or moved-from) carries no connection; any operation on it throws
 * HttpRequestException{Stage::Unknown}. Synchronous members throw directly;
 * awaitable members surface the exception through the awaitable when it is
 * co_awaited — so a driver's existing catch-all enriches it as usual.
 *
 * Exclusive ownership as on the concrete flavours: move-only, never shared
 * across threads.
 */
class connection_stream {
public:
    /// The empty state: no connection. Operations on it throw.
    connection_stream() noexcept = default;

    /// Adopt a connected TLS stream; a null pointer degenerates to empty.
    connection_stream(std::unique_ptr<https_stream> stream) noexcept {
        if (stream) _alternative = std::move(stream);
    }

    /// Adopt a connected plain stream; a null pointer degenerates to empty.
    connection_stream(std::unique_ptr<http_stream> stream) noexcept {
        if (stream) _alternative = std::move(stream);
    }

    // One connection, one owner: moving re-homes it, copying is meaningless.
    // Hand-written rather than defaulted: a defaulted move would leave the
    // variant's previously-active alternative holding a moved-null pointer,
    // which empty() does not recognize — every guard would then let the
    // dispatch dereference that null. Parking the source back in monostate
    // keeps the documented contract: a moved-from stream IS an empty stream.
    connection_stream(connection_stream&& other) noexcept
        : _alternative(std::move(other._alternative))
    {
        other._alternative = std::monostate{};
    }

    connection_stream& operator=(connection_stream&& other) noexcept {
        if (this != &other) {
            _alternative = std::move(other._alternative);
            other._alternative = std::monostate{};
        }
        return *this;
    }
    connection_stream(const connection_stream&) = delete;
    connection_stream& operator=(const connection_stream&) = delete;
    ~connection_stream() = default;

    /// Whether this value carries a connection (false: empty/moved-from).
    bool empty() const noexcept {
        return std::holds_alternative<std::monostate>(_alternative);
    }

    /// Whether the held connection is the TLS flavour. Diagnostics/tests.
    /// @throws HttpRequestException{Stage::Unknown} when empty.
    bool is_tls() const {
        _check("is_tls");
        return std::holds_alternative<std::unique_ptr<https_stream>>(
            _alternative);
    }

    // --- deadline controls (lowest layer; see https_stream.hpp) -----------

    /// Arm the connect/write/read deadline on the lowest layer.
    /// @throws HttpRequestException{Stage::Unknown} when empty.
    void expires_after(std::chrono::steady_clock::duration expiry) {
        _check("expires_after");
        std::visit(
            [&](auto& alternative) {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (!std::is_same_v<Flavour, std::monostate>) {
                    boost::beast::get_lowest_layer(*alternative)
                        .expires_after(expiry);
                }
            },
            _alternative);
    }

    /// Disarm the deadline (the SSE body phase's indefinite wait).
    /// @throws HttpRequestException{Stage::Unknown} when empty.
    void expires_never() {
        _check("expires_never");
        std::visit(
            [&](auto& alternative) {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (!std::is_same_v<Flavour, std::monostate>) {
                    boost::beast::get_lowest_layer(*alternative)
                        .expires_never();
                }
            },
            _alternative);
    }

    // --- HTTP exchange operations -----------------------------------------

    /**
     * @brief Write one complete HTTP request (headers + body).
     * @throws HttpRequestException{Stage::Unknown} when empty;
     *         boost::system::system_error on write/timeout failure.
     */
    template<typename Body>
    boost::asio::awaitable<void> write(
        const boost::beast::http::request<Body>& request) {
        _check("write");
        co_await std::visit(
            [&request](auto& alternative)
                -> boost::asio::awaitable<void> {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Flavour, std::monostate>) {
                    _fail("write");   // unreachable: _check rejected empty
                } else {
                    co_await boost::beast::http::async_write(
                        *alternative, request, boost::asio::use_awaitable);
                }
            },
            _alternative);
        co_return;
    }

    /**
     * @brief Read the response header only (buffer-body parser; the body is
     *        delivered incrementally through read_some).
     * @throws HttpRequestException{Stage::Unknown} when empty;
     *         boost::system::system_error on read/timeout failure.
     */
    boost::asio::awaitable<void> read_header(
        boost::beast::flat_buffer& buffer,
        boost::beast::http::response_parser<
            boost::beast::http::buffer_body>& parser) {
        _check("read_header");
        co_await std::visit(
            [&buffer, &parser](auto& alternative)
                -> boost::asio::awaitable<void> {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Flavour, std::monostate>) {
                    _fail("read_header");
                } else {
                    co_await boost::beast::http::async_read_header(
                        *alternative, buffer, parser,
                        boost::asio::use_awaitable);
                }
            },
            _alternative);
        co_return;
    }

    /**
     * @brief Pull the next body chunk into the parser's buffer body.
     *
     * Reported through the returned error_code rather than an exception:
     * the caller classifies the benign/terminal codes (need_buffer, eof,
     * partial_message, stream_truncated) exactly as it did over the raw
     * stream — that classification IS sse_request's end-of-stream logic.
     *
     * @throws HttpRequestException{Stage::Unknown} when empty.
     */
    boost::asio::awaitable<boost::system::error_code> read_some(
        boost::beast::flat_buffer& buffer,
        boost::beast::http::response_parser<
            boost::beast::http::buffer_body>& parser) {
        _check("read_some");
        boost::system::error_code ec;
        co_await std::visit(
            [&buffer, &parser, &ec](auto& alternative)
                -> boost::asio::awaitable<void> {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Flavour, std::monostate>) {
                    _fail("read_some");
                } else {
                    co_await boost::beast::http::async_read_some(
                        *alternative, buffer, parser,
                        boost::asio::redirect_error(
                            boost::asio::use_awaitable, ec));
                }
            },
            _alternative);
        co_return ec;
    }

    /**
     * @brief Read one complete response (header + whole body) into @p parser.
     * @throws HttpRequestException{Stage::Unknown} when empty;
     *         boost::system::system_error on read/timeout failure — the
     *         caller maps timeout codes to HttpRequestTimeoutException.
     */
    template<typename Body>
    boost::asio::awaitable<void> read(
        boost::beast::flat_buffer& buffer,
        boost::beast::http::response_parser<Body>& parser) {
        _check("read");
        co_await std::visit(
            [&buffer, &parser](auto& alternative)
                -> boost::asio::awaitable<void> {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Flavour, std::monostate>) {
                    _fail("read");
                } else {
                    co_await boost::beast::http::async_read(
                        *alternative, buffer, parser,
                        boost::asio::use_awaitable);
                }
            },
            _alternative);
        co_return;
    }

    // --- teardown ----------------------------------------------------------

    /**
     * @brief Tear the connection down, flavour-appropriately: best-effort
     *        TLS close_notify for the TLS flavour, a plain FIN otherwise,
     *        then the lowest-layer close. Never throws for I/O reasons.
     * @throws HttpRequestException{Stage::Unknown} when empty.
     */
    boost::asio::awaitable<void> shutdown() {
        _check("shutdown");
        co_await std::visit(
            [](auto& alternative) -> boost::asio::awaitable<void> {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Flavour, std::monostate>) {
                    _fail("shutdown");
                } else {
                    co_await shutdown_stream(*alternative);
                }
            },
            _alternative);
        co_return;
    }

    /// Close the socket outright, best-effort, never throws. Test teardown
    /// and destructor-adjacent paths; empty is a no-op.
    void close() noexcept {
        std::visit(
            [](auto& alternative) {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (!std::is_same_v<Flavour, std::monostate>) {
                    boost::beast::get_lowest_layer(*alternative).close();
                }
            },
            _alternative);
    }

private:
    // Empty-stream guard: the facade's replacement for the drivers' former
    // null-pointer check — same exception type, same stage, so a caller's
    // catch site cannot tell the difference.
    void _check(const char* operation) const {
        if (empty()) {
            throw HttpRequestException(
                HttpRequestException::Stage::Unknown,
                std::string{"connection_stream: "} + operation +
                    " on an empty stream (no connection)");
        }
    }

    // The unreachable empty branch inside visit lambdas (visit instantiates
    // the lambda for EVERY alternative; _check has already rejected empty).
    [[noreturn]] static void _fail(const char* operation) {
        throw HttpRequestException(
            HttpRequestException::Stage::Unknown,
            std::string{"connection_stream: "} + operation +
                " on an empty stream (no connection)");
    }

    // monostate = empty (no connection); never held alongside an alternative.
    std::variant<
        std::monostate,
        std::unique_ptr<https_stream>,
        std::unique_ptr<http_stream>>
        _alternative;
};

} // namespace endpoint
