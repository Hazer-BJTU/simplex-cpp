#define BOOST_TEST_MODULE HttpRequestExceptionTests
#include <boost/test/unit_test.hpp>

#include "endpoint/http_request_exception.hpp"

#include <boost/system/errc.hpp>

BOOST_AUTO_TEST_CASE(retains_failure_context)
{
    const auto ec = make_error_code(boost::system::errc::connection_refused);
    const HttpRequestException exception(
        HttpRequestException::Stage::Write,
        "request write failed",
        ec,
        "POST",
        "/v1/messages",
        "example.com");

    BOOST_CHECK(exception.stage() == HttpRequestException::Stage::Write);
    BOOST_TEST(exception.what() == std::string("request write failed"));
    BOOST_TEST(exception.error_code() == ec);
    BOOST_TEST(exception.method() == "POST");
    BOOST_TEST(exception.target() == "/v1/messages");
    BOOST_TEST(exception.host() == "example.com");
}

BOOST_AUTO_TEST_CASE(optional_context_defaults_to_empty)
{
    const HttpRequestException exception(
        HttpRequestException::Stage::Unknown, "unknown failure");

    BOOST_TEST(!exception.error_code());
    BOOST_TEST(exception.method().empty());
    BOOST_TEST(exception.target().empty());
    BOOST_TEST(exception.host().empty());
}
