#pragma once

//
// websocket_stream.hpp — the runtime-flavour WebSocket connection facade
// ======================================================================
//
// The intercom module's equivalent of endpoint::connection_stream, one level
// up the protocol stack: where connection_stream owns an HTTP exchange over
// either TLS or plain TCP, websocket_stream owns a WebSocket SESSION over
// either flavour, behind one move-only value:
//
//   ws_tls_stream   = websocket::stream<endpoint::https_stream>   (wss://)
//   ws_plain_stream = websocket::stream<endpoint::http_stream>    (ws://)
//
// The flavour is a RUNTIME fact (the endpoint's scheme), exactly as on the
// HTTP side, so a std::variant dispatches each operation instead of a
// template parameter. The canonical producer is connect_websocket below: it
// reuses endpoint's connect_flavour to establish the TCP/TLS connection (SNI,
// certificate verification, handshake — all unchanged from the HTTP side),
// wraps the connected stream in a websocket::stream, and completes the
// WebSocket upgrade handshake. A caller that wants the plain connection
// flavour passes tls=false in the ResolvedEndpoint; connect_websocket returns
// the same facade type either way.
//
// Message framing: the module's contract is TEXT frames (the payloads are
// JSON in practice). write() sets the text option explicitly and sends one
// complete message; read() reads one complete message — reassembling any
// fragmentation the peer used — and leaves the payload in the caller's
// flat_buffer, whatever the peer's frame type was. Ownership discipline is
// unchanged from the underlying flavours: one owner per live session, moved
// never shared.

#include <chrono>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>   // async_teardown for ssl::stream

#include "endpoint/model_request.hpp"   // ResolvedEndpoint, connect_flavour, stream aliases
#include "intercom/ws_exception.hpp"

// Teardown customization for Beast's timeout-aware tcp_stream. Beast ships
// async_teardown overloads for basic_stream_socket and ssl::stream but NOT
// for tcp_stream — the timeout-aware wrapper endpoint's plain flavour is
// built on — so websocket::stream<tcp_stream>::async_close would otherwise
// fall through to the generic overload and static_assert. This is the
// documented extension point: async_close resolves
// async_teardown(role, next_layer, handler) here, and the ssl::stream
// overload (ssl.hpp) forwards to this same overload for its own next layer.
// The forwarding is trivial — delegate to the built-in basic_stream_socket
// overload, which shutdown_send's the client role then closes.
namespace boost {
namespace beast {
namespace websocket {

template<class TeardownHandler>
void async_teardown(
    role_type role,
    boost::beast::tcp_stream& stream,
    TeardownHandler&& handler)
{
    async_teardown(role, stream.socket(),
                   std::forward<TeardownHandler>(handler));
}

} // namespace websocket
} // namespace beast
} // namespace boost

namespace intercom {

/// The TLS flavour: WebSocket over the module's verified HTTPS stream.
using ws_tls_stream =
    boost::beast::websocket::stream<endpoint::https_stream>;
/// The plain flavour: WebSocket over the module's plain TCP stream.
using ws_plain_stream =
    boost::beast::websocket::stream<endpoint::http_stream>;

/**
 * @brief One live WebSocket session of either flavour, behind a move-only
 *        value.
 *
 * Dispatches each operation to the held flavour at runtime. An empty stream
 * (default-constructed, moved-from, or adopted from a null pointer) carries
 * no session; any operation on it throws WsException{Stage::Unknown}.
 * Synchronous members throw directly; awaitable members surface the
 * exception through the awaitable when co_awaited.
 */
class websocket_stream {
public:
    /// The empty state: no session. Operations on it throw.
    websocket_stream() noexcept = default;

    /// Adopt a connected wss:// session; a null pointer degenerates to empty.
    websocket_stream(std::unique_ptr<ws_tls_stream> stream) noexcept {
        if (stream) _alternative = std::move(stream);
    }

    /// Adopt a connected ws:// session; a null pointer degenerates to empty.
    websocket_stream(std::unique_ptr<ws_plain_stream> stream) noexcept {
        if (stream) _alternative = std::move(stream);
    }

    // One session, one owner: moving re-homes it, copying is meaningless.
    // Hand-written so a moved-from source parks back in monostate — a
    // defaulted move would leave a moved-null unique_ptr in the variant,
    // which empty() does not recognize.
    websocket_stream(websocket_stream&& other) noexcept
        : _alternative(std::move(other._alternative))
    {
        other._alternative = std::monostate{};
    }

    websocket_stream& operator=(websocket_stream&& other) noexcept {
        if (this != &other) {
            _alternative = std::move(other._alternative);
            other._alternative = std::monostate{};
        }
        return *this;
    }
    websocket_stream(const websocket_stream&) = delete;
    websocket_stream& operator=(const websocket_stream&) = delete;
    ~websocket_stream() = default;

    /// Whether this value carries a session (false: empty/moved-from).
    bool empty() const noexcept {
        return std::holds_alternative<std::monostate>(_alternative);
    }

    /// Whether the held session is the TLS flavour. Diagnostics/tests.
    /// @throws WsException{Stage::Unknown} when empty.
    bool is_tls() const {
        _check("is_tls");
        return std::holds_alternative<std::unique_ptr<ws_tls_stream>>(
            _alternative);
    }

    // --- deadline controls (lowest layer) ---------------------------------

    /// Arm the deadline on the lowest layer (the tcp_stream).
    /// @throws WsException{Stage::Unknown} when empty.
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

    /// Disarm the deadline.
    /// @throws WsException{Stage::Unknown} when empty.
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

    // --- message operations ------------------------------------------------

    /**
     * @brief Send one complete TEXT message (the module's contract — the
     *        payloads are JSON).
     *
     * @throws WsException{Stage::Unknown} when empty;
     *         boost::system::system_error on write/timeout failure.
     */
    boost::asio::awaitable<void> write(std::string message) {
        _check("write");
        co_await std::visit(
            [&message](auto& alternative) -> boost::asio::awaitable<void> {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Flavour, std::monostate>) {
                    _fail("write");   // unreachable: _check rejected empty
                } else {
                    // Explicit: the module speaks text (JSON) by default.
                    alternative->text(true);
                    co_await alternative->async_write(
                        boost::asio::buffer(message),
                        boost::asio::use_awaitable);
                }
            },
            _alternative);
        co_return;
    }

    /**
     * @brief Read one complete message into @p buffer, reassembling any
     *        fragmentation the peer used.
     *
     * The payload — text or binary, whatever the peer sent — lands in the
     * flat_buffer; the caller renders it with boost::beast::buffers_to_string.
     *
     * @throws WsException{Stage::Unknown} when empty;
     *         boost::system::system_error on read/timeout failure.
     */
    boost::asio::awaitable<void> read(boost::beast::flat_buffer& buffer) {
        _check("read");
        co_await std::visit(
            [&buffer](auto& alternative) -> boost::asio::awaitable<void> {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Flavour, std::monostate>) {
                    _fail("read");
                } else {
                    co_await alternative->async_read(
                        buffer, boost::asio::use_awaitable);
                }
            },
            _alternative);
        co_return;
    }

    /**
     * @brief Best-effort close handshake (close_code::normal).
     *
     * A peer that already dropped the session answers with a close error,
     * which is swallowed — the socket teardown in the destructor suffices.
     * @throws WsException{Stage::Unknown} when empty.
     */
    boost::asio::awaitable<void> close() {
        _check("close");
        co_await std::visit(
            [](auto& alternative) -> boost::asio::awaitable<void> {
                using Flavour = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Flavour, std::monostate>) {
                    _fail("close");
                } else {
                    boost::system::error_code ec;
                    co_await alternative->async_close(
                        boost::beast::websocket::close_code::normal,
                        boost::asio::redirect_error(
                            boost::asio::use_awaitable, ec));
                }
            },
            _alternative);
        co_return;
    }

private:
    // Empty-session guard: same exception type and stage as the facade's
    // other failure paths, so a caller's catch site cannot tell the guard
    // from a driver-level bug.
    void _check(const char* operation) const {
        if (empty()) {
            throw WsException(
                WsException::Stage::Unknown,
                std::string{"intercom::websocket_stream: "} + operation +
                    " on an empty stream (no connection)");
        }
    }

    // The unreachable empty branch inside visit lambdas (visit instantiates
    // the lambda for EVERY alternative; _check has already rejected empty).
    [[noreturn]] static void _fail(const char* operation) {
        throw WsException(
            WsException::Stage::Unknown,
            std::string{"intercom::websocket_stream: "} + operation +
                " on an empty stream (no connection)");
    }

    // monostate = empty; never held alongside an alternative.
    std::variant<
        std::monostate,
        std::unique_ptr<ws_tls_stream>,
        std::unique_ptr<ws_plain_stream>>
        _alternative;
};

/**
 * @brief Establish a WebSocket session: connect, upgrade, fold failures.
 *
 * The connect primitive every intercom exchange rides on. It reuses
 * endpoint::detail::connect_flavour — the SAME TCP+TLS connection factory
 * (SNI, certificate verification, handshake) the HTTP side uses — rather
 * than re-implementing it, then wraps the connected stream in a
 * websocket::stream and completes the upgrade handshake against
 * resolved.target (the WebSocket path) and resolved.host (the Host header).
 *
 * Every failure — DNS/TCP/TLS from connect_flavour, and the upgrade
 * handshake's own errors (a 401/403 rejection surfaces as
 * websocket::error::upgrade_declined) — is folded into
 * WsException{Stage::Connect} carrying the transport error_code and the
 * host/target context, so callers classify connect failures uniformly.
 *
 * @param executor Executor on which all connect/handshake I/O runs.
 * @param resolved Where to connect: host/port/tls as endpoint parsed them;
 *                 resolved.target is the WebSocket path.
 * @param context  TLS client context; wss:// flavour only, global by default.
 * @return A fully handshaken websocket_stream of the resolved scheme's
 *         flavour.
 * @throws WsException{Stage::Connect} on any connect or upgrade failure.
 */
inline boost::asio::awaitable<websocket_stream> connect_websocket(
    boost::asio::any_io_executor executor,
    const endpoint::ResolvedEndpoint& resolved,
    endpoint::ssl_context& context = endpoint::get_global_ssl_context())
{
    try {
        if (resolved.tls) {
            auto tls = co_await endpoint::detail::connect_flavour<
                endpoint::https_stream>::connect(
                    executor, resolved.host, resolved.port, context);
            ws_tls_stream ws{std::move(*tls)};
            boost::beast::get_lowest_layer(ws).expires_after(
                std::chrono::seconds(endpoint::DEFAULT_TIMEOUT_SEC));
            co_await ws.async_handshake(
                resolved.host, resolved.target, boost::asio::use_awaitable);
            co_return websocket_stream{
                std::make_unique<ws_tls_stream>(std::move(ws))};
        }
        auto plain = co_await endpoint::detail::connect_flavour<
            endpoint::http_stream>::connect(
                executor, resolved.host, resolved.port, context);
        ws_plain_stream ws{std::move(*plain)};
        boost::beast::get_lowest_layer(ws).expires_after(
            std::chrono::seconds(endpoint::DEFAULT_TIMEOUT_SEC));
        co_await ws.async_handshake(
            resolved.host, resolved.target, boost::asio::use_awaitable);
        co_return websocket_stream{
            std::make_unique<ws_plain_stream>(std::move(ws))};
    } catch (const boost::system::system_error& e) {
        throw WsException(
            WsException::Stage::Connect, e.what(), e.code(),
            resolved.host, resolved.target);
    } catch (const std::exception& e) {
        throw WsException(
            WsException::Stage::Connect, e.what(), {},
            resolved.host, resolved.target);
    } catch (...) {
        throw WsException(
            WsException::Stage::Connect, "unknown error", {},
            resolved.host, resolved.target);
    }
}

} // namespace intercom
