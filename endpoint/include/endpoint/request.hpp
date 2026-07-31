#pragma once

#include "endpoint/https_stream.hpp"
#include "exceptions/http_request_exception.hpp"

#include <boost/beast/http.hpp>
#include <boost/none.hpp>
#include <boost/system/system_error.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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

}
