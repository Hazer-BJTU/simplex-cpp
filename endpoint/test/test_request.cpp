#define BOOST_TEST_MODULE RequestTests
#include <boost/test/unit_test.hpp>

#include "endpoint/request.hpp"

#include <boost/asio.hpp>

#include <future>
#include <memory>

BOOST_AUTO_TEST_CASE(null_stream_is_reported_as_unknown_failure)
{
    std::unique_ptr<endpoint::https_stream> stream;
    auto request_handler = [](const nlohmann::json&) {
        return boost::beast::http::request<boost::beast::http::string_body>{};
    };
    auto response_handler = [](auto&&) { return nlohmann::json::object(); };

    boost::asio::io_context io;
    auto operation = endpoint::json_request_once(
        stream, nlohmann::json::object(), request_handler, response_handler);
    auto result = boost::asio::co_spawn(
        io, std::move(operation), boost::asio::use_future);
    io.run();

    try {
        result.get();
        BOOST_FAIL("expected HttpRequestException");
    } catch (const HttpRequestException& exception) {
        BOOST_CHECK(
            exception.stage() == HttpRequestException::Stage::Unknown);
        BOOST_TEST(exception.method().empty());
        BOOST_TEST(exception.target().empty());
        BOOST_TEST(exception.host().empty());
    }
}
