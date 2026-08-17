#define BOOST_TEST_MODULE HttpRequestExceptionTests
#include <boost/test/unit_test.hpp>

#include "endpoint/http_request_exception.hpp"

#include <boost/beast/core.hpp>
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

BOOST_AUTO_TEST_CASE(timeout_flavour_is_stage_read_and_catchable_as_base)
{
    const auto ec = make_error_code(boost::beast::error::timeout);
    const HttpRequestTimeoutException exception(
        "read timed out", ec, "POST", "/v1/responses", "example.com");

    // Always the read stage, no matter how it is caught.
    BOOST_CHECK(exception.stage() == HttpRequestException::Stage::Read);
    BOOST_TEST(exception.error_code() == ec);
    BOOST_TEST(exception.method() == "POST");

    // The dedicated catch works, and so does every existing handler written
    // against the base class.
    const HttpRequestException& as_base = exception;
    BOOST_CHECK(as_base.stage() == HttpRequestException::Stage::Read);
    try {
        throw HttpRequestTimeoutException("read timed out");
    } catch (const HttpRequestException& caught) {
        BOOST_TEST(caught.what() == std::string("read timed out"));
    }
}
