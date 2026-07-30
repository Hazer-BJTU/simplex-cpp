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
 */
using https_stream = boost::asio::ssl::stream<boost::beast::tcp_stream>;
using ssl_context = boost::asio::ssl::context;

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
/// Standard service port used by HTTPS.
inline constexpr std::string_view DEFAULT_HTTPS_PORT = "443";

/**
 * @brief Resolve a host, establish TCP, and complete a verified TLS handshake.
 *
 * SNI is set before the handshake, and the certificate is checked against
 * `host`. The returned shared pointer keeps the stream alive across subsequent
 * asynchronous HTTP operations.
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
inline boost::asio::awaitable<std::shared_ptr<https_stream>>
create_https_connection_stream(
    boost::asio::any_io_executor executor,
    ssl_context& context,
    std::string host,
    std::string_view port = DEFAULT_HTTPS_PORT
) {
    auto resolver = boost::asio::ip::tcp::resolver{ executor };
    auto stream = std::make_shared<https_stream>(executor, context);

    // SNI lets a server select the correct certificate on shared endpoints.
    if (!SSL_set_tlsext_host_name(stream->native_handle(), host.c_str())) {
        throw boost::beast::system_error(
            static_cast<int>(::ERR_get_error()),
            boost::asio::error::get_ssl_category());
    }
    // SNI alone is not authentication: verify the certificate's SAN/CN too.
    stream->set_verify_callback(boost::asio::ssl::host_name_verification(host));

    const auto resolve_result = co_await resolver.async_resolve(host, port, boost::asio::use_awaitable);

    boost::beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(DEFAULT_TIMEOUT_SEC));
    co_await boost::beast::get_lowest_layer(*stream).async_connect(resolve_result, boost::asio::use_awaitable);

    boost::beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(DEFAULT_TIMEOUT_SEC));
    co_await stream->async_handshake(boost::asio::ssl::stream_base::client);

    co_return stream;
}

/**
 * @brief Connect using the verified process-wide TLS context.
 *
 * This is the normal entry point for public HTTPS services. See the context
 * overload for parameter and error details.
 */
inline boost::asio::awaitable<std::shared_ptr<https_stream>>
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
inline boost::asio::awaitable<std::shared_ptr<https_stream>>
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
inline boost::asio::awaitable<std::shared_ptr<https_stream>>
create_https_connection_stream(
    std::string host,
    std::string_view port = DEFAULT_HTTPS_PORT
) {
    auto executor = co_await boost::asio::this_coro::executor;
    co_return co_await create_https_connection_stream(
        executor, get_global_ssl_context(), std::move(host), port);
}

} // namespace endpoint
