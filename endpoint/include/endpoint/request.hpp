#pragma once

#include "endpoint/https_stream.hpp"
#include "exceptions/http_request_exception.hpp"

#include <boost/beast/http.hpp>
#include <boost/system/system_error.hpp>

#include <nlohmann/json.hpp>

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace endpoint {

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
        co_await http::async_write(
            *stream,
            request,
            boost::asio::use_awaitable);

        // Read one complete HTTP response. The buffer only needs to live for
        // this operation because Beast transfers the body into response.
        stage = HttpRequestException::Stage::Read;
        boost::beast::flat_buffer buffer;
        http::response<http::dynamic_body> response;
        co_await http::async_read(
            *stream,
            buffer,
            response,
            boost::asio::use_awaitable);

        // Let the caller validate and decode the service-specific response.
        stage = HttpRequestException::Stage::HandleResponse;
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

}
