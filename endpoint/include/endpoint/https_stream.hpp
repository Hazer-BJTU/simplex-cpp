#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace endpoint {

/**
 * @brief TLS client stream built on Beast's timeout-aware TCP stream.
 *
 * The lowest layer can be accessed with `boost::beast::get_lowest_layer()`
 * when callers need to change a deadline or close the socket.
 *
 * Ownership of a live connection is always exclusive: `create_https_connection_stream`
 * returns a `std::unique_ptr<https_stream>`, and the underlying `ssl::stream` is
 * non-copyable. A connection therefore has a single owner at any time and may only
 * be handed off by moving it. This guarantees a stream is never shared across
 * threads — any future pooling or transfer logic must preserve this move-only
 * discipline and never expose shared access to one connection.
 */
using https_stream = boost::asio::ssl::stream<boost::beast::tcp_stream>;
using ssl_context = boost::asio::ssl::context;

/**
 * @brief Plain-HTTP client stream: Beast's timeout-aware TCP stream without a
 *        TLS layer on top.
 *
 * Everything the module's HTTP machinery does through
 * `boost::beast::get_lowest_layer()` — deadlines, cancellation, teardown —
 * applies to this flavour unchanged, because `get_lowest_layer` returns the
 * tcp_stream itself. The same exclusive-ownership discipline as `https_stream`
 * applies: `create_http_connection_stream` returns a `std::unique_ptr`, and a
 * connection is moved, never shared across threads.
 */
using http_stream = boost::beast::tcp_stream;

/**
 * @brief Return the process-wide TLS client context.
 *
 * The context loads the operating system's default trust store and requires
 * peer-certificate verification. Initialization is thread-safe. The returned
 * object is owned by the process and must not be destroyed by the caller.
 *
 * @throws boost::system::system_error if the system trust store cannot be
 *         configured.
 */
inline ssl_context& get_global_ssl_context() {
    static std::once_flag initialize_ssl_context_flag;
    static std::unique_ptr<ssl_context> global_ssl_context;
    std::call_once(initialize_ssl_context_flag, []() -> void {
        // tls_client negotiates the best protocol supported by both peers
        // (including TLS 1.3), rather than artificially pinning TLS 1.2.
        global_ssl_context =
            std::make_unique<ssl_context>(boost::asio::ssl::context::tls_client);
        global_ssl_context->set_default_verify_paths();
        global_ssl_context->set_verify_mode(boost::asio::ssl::verify_peer);
    });
    return *global_ssl_context;
}

/// Deadline applied independently to the TCP connect and TLS handshake.
inline constexpr std::size_t DEFAULT_TIMEOUT_SEC = 30;
/// Deadline for writing one complete request (headers + body). Deliberately
/// longer than the connect/handshake deadline so a large request body — a
/// multi-MB conversation context over a slow uplink — is not prematurely
/// treated as unreachable.
inline constexpr std::size_t DEFAULT_WRITE_TIMEOUT_SEC = 60;
/// Default read deadline for a bounded (non-SSE) HTTP response — deliberately
/// far above DEFAULT_TIMEOUT_SEC: a model backend may compute for minutes
/// before its first response byte. Per-call configurable on http_request,
/// where 0 disables the deadline.
inline constexpr std::size_t DEFAULT_HTTP_READ_TIMEOUT_SEC = 300;
/// Standard service port used by HTTPS.
inline constexpr std::string_view DEFAULT_HTTPS_PORT = "443";
/// Standard service port used by plain HTTP.
inline constexpr std::string_view DEFAULT_HTTP_PORT = "80";

/**
 * @brief Resolve a host and complete the TCP connection on a stream's lowest
 *        layer with the standard connect deadline.
 *
 * The shared prefix of the connection factories below — the unified
 * `create_connection_stream` and its flavour-specific wrappers: it applies to
 * any stream whose lowest layer is a timeout-aware `boost::beast::tcp_stream`,
 * which is what both `https_stream` and `http_stream` are built on
 * (`get_lowest_layer` returns the tcp_stream itself for a bare `http_stream`).
 *
 * @param executor Executor on which DNS and socket operations run.
 * @param stream   Stream whose lowest layer receives the connection.
 * @param host     DNS name used for resolution.
 * @param port     Numeric port or service name.
 * @throws boost::system::system_error on DNS, TCP, or timeout failure.
 */
template<typename Stream>
boost::asio::awaitable<void> connect_tcp(
    boost::asio::any_io_executor executor,
    Stream& stream,
    const std::string& host,
    std::string_view port)
{
    auto resolver = boost::asio::ip::tcp::resolver{ executor };
    const auto endpoints = co_await resolver.async_resolve(host, port, boost::asio::use_awaitable);

    boost::beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(DEFAULT_TIMEOUT_SEC));
    co_await boost::beast::get_lowest_layer(stream).async_connect(endpoints, boost::asio::use_awaitable);
}

/**
 * @brief Whether a connection-stream flavour carries a TLS layer.
 *
 * Detected the same way `shutdown_stream` specialises teardown: a stream that
 * can perform an SSL client handshake — `https_stream` — is TLS-flavoured; a
 * bare `http_stream` has no such operation and is not. `if constexpr` on this
 * trait is what lets one factory body serve both flavours.
 */
template<typename Stream>
inline constexpr bool is_tls_stream_v =
    requires(Stream& stream) {
        stream.async_handshake(
            boost::asio::ssl::stream_base::client, boost::asio::use_awaitable);
    };

/**
 * @brief The flavour's standard service port: 443 for TLS, 80 for plain.
 */
template<typename Stream>
inline constexpr std::string_view default_connection_port_v =
    is_tls_stream_v<Stream> ? DEFAULT_HTTPS_PORT : DEFAULT_HTTP_PORT;

/**
 * @brief Resolve a host and establish a connection of either stream flavour.
 *
 * The single implementation behind `create_https_connection_stream` and
 * `create_http_connection_stream`, selected at compile time by @p Stream:
 * instantiate with `https_stream` for the full TLS path — SNI before the
 * handshake, certificate verification against @p host, then the handshake —
 * or with `http_stream` for plain TCP only. Generic code that is already
 * parameterised on its stream flavour can create its connections here without
 * naming a flavour-specific factory.
 *
 * @p context participates only in the TLS flavour, where it must outlive the
 * stream; the plain flavour accepts and ignores it, so one call shape serves
 * both. The default is the process-wide verified context.
 *
 * The returned `std::unique_ptr` grants the caller exclusive ownership of the
 * stream; it keeps the connection alive across subsequent asynchronous HTTP
 * operations and must be moved (never copied) when transferred between scopes
 * or coroutines, so the connection is never shared across threads. The connect
 * deadline stays armed until the caller sets its own.
 *
 * @tparam Stream  https_stream, http_stream, or another stream whose lowest
 *                 layer is a timeout-aware `boost::beast::tcp_stream`.
 * @param executor Executor on which DNS and socket operations run.
 * @param host     DNS name used for resolution, SNI, and certificate
 *                 verification (the latter two TLS flavour only).
 * @param port     Numeric port or service name; defaults to the flavour's
 *                 standard port.
 * @param context  TLS client context; TLS flavour only.
 * @return A connected stream, with the TLS client handshake completed for
 *         https_stream.
 * @throws boost::system::system_error on SNI, DNS, TCP, timeout, certificate,
 *         or TLS-handshake failure, as applicable to the flavour.
 */
template<typename Stream>
boost::asio::awaitable<std::unique_ptr<Stream>>
create_connection_stream(
    boost::asio::any_io_executor executor,
    std::string host,
    std::string_view port = default_connection_port_v<Stream>,
    [[maybe_unused]] ssl_context& context = get_global_ssl_context())
{
    std::unique_ptr<Stream> stream;
    if constexpr (is_tls_stream_v<Stream>) {
        stream = std::make_unique<Stream>(executor, context);

        // SNI lets a server select the correct certificate on shared endpoints.
        if (!SSL_set_tlsext_host_name(stream->native_handle(), host.c_str())) {
            throw boost::beast::system_error(
                static_cast<int>(::ERR_get_error()),
                boost::asio::error::get_ssl_category());
        }
        // SNI alone is not authentication: verify the certificate's SAN/CN too.
        stream->set_verify_callback(
            boost::asio::ssl::host_name_verification(host));
    } else {
        stream = std::make_unique<Stream>(executor);
    }

    co_await connect_tcp(executor, *stream, host, port);

    if constexpr (is_tls_stream_v<Stream>) {
        boost::beast::get_lowest_layer(*stream).expires_after(
            std::chrono::seconds(DEFAULT_TIMEOUT_SEC));
        co_await stream->async_handshake(
            boost::asio::ssl::stream_base::client);
    }

    co_return stream;
}

/**
 * @brief Connect on the calling coroutine's executor, either flavour.
 *
 * Convenience overload for callers already inside an Asio coroutine; same
 * parameters, in the same order, as the executor-taking overload.
 */
template<typename Stream>
boost::asio::awaitable<std::unique_ptr<Stream>>
create_connection_stream(
    std::string host,
    std::string_view port = default_connection_port_v<Stream>,
    [[maybe_unused]] ssl_context& context = get_global_ssl_context())
{
    auto executor = co_await boost::asio::this_coro::executor;
    co_return co_await create_connection_stream<Stream>(
        executor, std::move(host), port, context);
}

/**
 * @brief Resolve a host, establish TCP, and complete a verified TLS handshake.
 *
 * The TLS flavour of `create_connection_stream` — a thin wrapper kept for
 * flavour-specific call sites — so SNI, verification against `host`, the
 * handshake ordering, and the exclusive move-only ownership of the returned
 * `std::unique_ptr` are documented there.
 *
 * This overload accepts an explicit context, primarily for applications with a
 * private CA or a custom trust policy. The context must outlive the stream.
 *
 * @param executor Executor on which DNS and socket operations run.
 * @param context Configured TLS client context.
 * @param host DNS name used for resolution, SNI, and certificate verification.
 * @param port Numeric port or service name.
 * @return A connected stream with a completed TLS client handshake.
 * @throws boost::system::system_error on SNI, DNS, TCP, timeout, certificate,
 *         or TLS-handshake failure.
 */
inline boost::asio::awaitable<std::unique_ptr<https_stream>>
create_https_connection_stream(
    boost::asio::any_io_executor executor,
    ssl_context& context,
    std::string host,
    std::string_view port = DEFAULT_HTTPS_PORT
) {
    co_return co_await create_connection_stream<https_stream>(
        executor, std::move(host), port, context);
}

/**
 * @brief Connect using the verified process-wide TLS context.
 *
 * This is the normal entry point for public HTTPS services. See the context
 * overload for parameter and error details.
 */
inline boost::asio::awaitable<std::unique_ptr<https_stream>>
create_https_connection_stream(
    boost::asio::any_io_executor executor,
    std::string host,
    std::string_view port = DEFAULT_HTTPS_PORT
) {
    co_return co_await create_https_connection_stream(
        executor, get_global_ssl_context(), std::move(host), port);
}

/**
 * @brief Connect on the calling coroutine's executor with an explicit context.
 *
 * This convenience overload must be called from an Asio coroutine. It obtains
 * that coroutine's executor with `boost::asio::this_coro::executor` and then
 * delegates to the executor-taking overload.
 */
inline boost::asio::awaitable<std::unique_ptr<https_stream>>
create_https_connection_stream(
    ssl_context& context,
    std::string host,
    std::string_view port = DEFAULT_HTTPS_PORT
) {
    auto executor = co_await boost::asio::this_coro::executor;
    co_return co_await create_https_connection_stream(
        executor, context, std::move(host), port);
}

/**
 * @brief Connect on the calling coroutine's executor using the global context.
 *
 * This is the shortest form for callers already executing inside an Asio
 * coroutine.
 */
inline boost::asio::awaitable<std::unique_ptr<https_stream>>
create_https_connection_stream(
    std::string host,
    std::string_view port = DEFAULT_HTTPS_PORT
) {
    auto executor = co_await boost::asio::this_coro::executor;
    co_return co_await create_https_connection_stream(
        executor, get_global_ssl_context(), std::move(host), port);
}

/**
 * @brief Resolve a host and establish a plain TCP connection (no TLS).
 *
 * The plain flavour of `create_connection_stream` — a thin wrapper kept for
 * flavour-specific call sites — for `http://` endpoints such as local model
 * backends. Ownership and deadline discipline are documented there; in
 * particular the connect deadline stays armed until the caller (e.g.
 * sse_request) sets its own.
 *
 * @param executor Executor on which DNS and socket operations run.
 * @param host     DNS name used for resolution.
 * @param port     Numeric port or service name.
 * @return A connected plain stream.
 * @throws boost::system::system_error on DNS, TCP, or timeout failure.
 */
inline boost::asio::awaitable<std::unique_ptr<http_stream>>
create_http_connection_stream(
    boost::asio::any_io_executor executor,
    std::string host,
    std::string_view port = DEFAULT_HTTP_PORT
) {
    co_return co_await create_connection_stream<http_stream>(
        executor, std::move(host), port);
}

/**
 * @brief Connect on the calling coroutine's executor.
 *
 * Convenience overload for callers already inside an Asio coroutine; see the
 * executor-taking overload for details.
 */
inline boost::asio::awaitable<std::unique_ptr<http_stream>>
create_http_connection_stream(
    std::string host,
    std::string_view port = DEFAULT_HTTP_PORT
) {
    auto executor = co_await boost::asio::this_coro::executor;
    co_return co_await create_http_connection_stream(
        executor, std::move(host), port);
}

/**
 * @brief Tear a connection stream down at the end of an exchange.
 *
 * Works for both stream flavours by asking the stream itself which teardown it
 * supports: a TLS-flavoured stream — one exposing `async_shutdown`, i.e.
 * `https_stream` — receives a best-effort `close_notify` (a peer may drop the
 * connection before answering it, so shutdown errors are captured and
 * ignored); a plain stream has no shutdown handshake, so the socket is merely
 * half-closed — the peer sees a FIN instead of a reset. Both branches finish
 * with the lowest-layer close, which is best-effort and never throws.
 *
 * @tparam Stream A stream whose lowest layer is a timeout-aware
 *                `boost::beast::tcp_stream` (both endpoint stream aliases
 *                qualify).
 */
template<typename Stream>
boost::asio::awaitable<void> shutdown_stream(Stream& stream) {
    boost::system::error_code ec;
    if constexpr (requires(Stream& s) { s.async_shutdown(boost::asio::use_awaitable); }) {
        co_await stream.async_shutdown(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    } else {
        boost::beast::get_lowest_layer(stream).socket().shutdown(
            boost::asio::ip::tcp::socket::shutdown_both, ec);
    }
    boost::beast::get_lowest_layer(stream).close();
    co_return;
}

} // namespace endpoint
