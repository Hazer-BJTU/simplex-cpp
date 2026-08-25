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
    BOOST_TEST(exception.status() == 0u);
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

BOOST_AUTO_TEST_CASE(to_string_renders_failure_context)
{
    const auto ec = make_error_code(boost::system::errc::connection_refused);
    const HttpRequestException exception(
        HttpRequestException::Stage::Write,
        "request write failed",
        ec,
        "POST",
        "/v1/messages",
        "example.com");

    // The expected ec text comes from the same error_code, so the assertion
    // stays locale-independent.
    BOOST_TEST(exception.to_string() ==
               "Failed while sending the request: request write failed (" +
                   ec.message() + "; POST /v1/messages to example.com)");
}

BOOST_AUTO_TEST_CASE(http_status_is_retained_and_rendered)
{
    const HttpRequestException exception(
        HttpRequestException::Stage::HandleResponse,
        "SSE request rejected",
        {},
        "POST",
        "/chat/completions",
        "api.deepseek.com",
        401);

    BOOST_TEST(exception.status() == 401u);
    BOOST_TEST(exception.to_string() ==
               "Failed while handling the response: SSE request rejected "
               "(HTTP status 401; POST /chat/completions to api.deepseek.com)");
}

BOOST_AUTO_TEST_CASE(connect_stage_renders_connection_failure)
{
    // The shape a connection-establishment failure takes: an error code and
    // the host, but no request line yet — nothing was sent.
    const auto ec = make_error_code(boost::system::errc::connection_refused);
    const HttpRequestException exception(
        HttpRequestException::Stage::Connect,
        "connection refused",
        ec,
        {},
        {},
        "example.com");

    BOOST_CHECK(exception.stage() == HttpRequestException::Stage::Connect);
    BOOST_TEST(exception.status() == 0u);
    BOOST_TEST(exception.to_string() ==
               "Failed while establishing the connection: connection refused (" +
                   ec.message() + "; to example.com)");
}

BOOST_AUTO_TEST_CASE(to_string_omits_absent_context)
{
    const HttpRequestException exception(
        HttpRequestException::Stage::Unknown, "unknown failure");

    BOOST_TEST(exception.to_string() == "Failed at an unknown stage: unknown failure");

    // The timeout flavour renders through the same path, stage pinned to read.
    BOOST_TEST(HttpRequestTimeoutException("read timed out").to_string() ==
               "Failed while reading the response: read timed out");
}
