#include "endpoint/connection_stream.hpp"
#include "endpoint/model_request.hpp"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <exception>
#include <iostream>
#include <string>

namespace asio = boost::asio;
namespace http = boost::beast::http;

asio::awaitable<unsigned> check_connection(std::string host)
{
    // TLS is the runtime choice implied by an https:// endpoint; the global
    // verified context is the default.
    endpoint::ResolvedEndpoint resolved{.host = host};
    endpoint::connection_stream stream =
        co_await endpoint::create_connection_stream(resolved);

    http::request<http::empty_body> request{http::verb::head, "/", 11};
    request.set(http::field::host, host);
    request.set(http::field::user_agent, "simplex-cpp-connectivity-check");
    stream.expires_after(std::chrono::seconds(30));
    co_await stream.write(request);

    boost::beast::flat_buffer buffer;
    // HEAD may advertise the GET representation's Content-Length while
    // carrying no body. Tell Beast not to wait for those nonexistent bytes.
    http::response_parser<http::empty_body> parser;
    parser.skip(true);
    stream.expires_after(std::chrono::seconds(30));
    co_await stream.read(buffer, parser);
    const auto status = parser.get().result_int();

    // A peer may omit close_notify, so shutdown errors are irrelevant after a
    // complete HTTP response. The handshake and request have already proved
    // end-to-end connectivity.
    co_await stream.shutdown();

    co_return status;
}

int main(int argc, char* argv[])
{
    const std::string host = argc > 1 ? argv[1] : "example.com";

    try {
        asio::io_context io;
        auto result = asio::co_spawn(
            io, check_connection(host), asio::use_future);
        io.run();
        const auto status = result.get();
        std::cout << host << " HTTPS status: " << status << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << host << " HTTPS check failed: " << error.what() << '\n';
        return 1;
    }
}
