#define BOOST_TEST_MODULE ProcessIoCapture
#include <boost/test/unit_test.hpp>

#include "process_io/capture.hpp"

using namespace process_io;

BOOST_AUTO_TEST_CASE(topology_distinguishes_absent_from_empty)
{
    CaptureBuffer pipes(OutputTopology::Separate, 4);
    BOOST_REQUIRE(pipes.view(Stream::Stdout));
    BOOST_TEST(pipes.view(Stream::Stdout)->bytes.empty());
    BOOST_REQUIRE(pipes.view(Stream::Stderr));
    BOOST_TEST(!pipes.view(Stream::Terminal));
    BOOST_TEST(!pipes.complete());
    BOOST_CHECK_THROW(pipes.append(Stream::Terminal, "x"), std::invalid_argument);

    CaptureBuffer terminal(OutputTopology::Terminal, 4);
    BOOST_TEST(!terminal.view(Stream::Stdout));
    BOOST_TEST(!terminal.view(Stream::Stderr));
    BOOST_REQUIRE(terminal.view(Stream::Terminal));
    terminal.append(Stream::Terminal, "once");
    BOOST_TEST(terminal.view(Stream::Terminal)->bytes == "once");
}

BOOST_AUTO_TEST_CASE(prefix_limit_counts_raw_bytes_and_preserves_current_per_stream_policy)
{
    CaptureBuffer capture(OutputTopology::Separate, 4);
    // Deliberately split UTF-8 and include NUL/control bytes: storage is raw.
    capture.append(Stream::Stdout, std::string_view("\xe4\xb8", 2));
    capture.append(Stream::Stdout, std::string_view("\xad\0", 2));
    BOOST_TEST(!capture.view(Stream::Stdout)->truncated);
    capture.append(Stream::Stdout, "\x1b[0m");
    BOOST_TEST(capture.view(Stream::Stdout)->bytes == std::string_view("\xe4\xb8\xad\0", 4));
    BOOST_TEST(capture.view(Stream::Stdout)->truncated);
    capture.append(Stream::Stderr, "abcd");
    BOOST_TEST(capture.view(Stream::Stderr)->bytes == "abcd");
    BOOST_TEST(!capture.view(Stream::Stderr)->truncated);
    capture.append(Stream::Stderr, "");
    BOOST_TEST(!capture.view(Stream::Stderr)->truncated);
}

BOOST_AUTO_TEST_CASE(unlimited_and_single_byte_limits)
{
    CaptureBuffer unlimited(OutputTopology::Terminal, 0);
    const std::string bytes(50000, 'z');
    unlimited.append(Stream::Terminal, bytes);
    unlimited.append(Stream::Terminal, bytes);
    BOOST_TEST(unlimited.view(Stream::Terminal)->bytes.size() == 100000U);
    BOOST_TEST(!unlimited.view(Stream::Terminal)->truncated);

    CaptureBuffer one(OutputTopology::Separate, 1);
    one.append(Stream::Stdout, "abcd");
    BOOST_TEST(one.view(Stream::Stdout)->bytes == "a");
    BOOST_TEST(one.view(Stream::Stdout)->truncated);
}

BOOST_AUTO_TEST_CASE(completion_keeps_eof_stop_and_failure_distinct)
{
    CaptureBuffer capture(OutputTopology::Separate, 0);
    capture.append(Stream::Stdout, "tail");
    capture.finish(Stream::Stdout, StreamEnd::Eof);
    BOOST_TEST(!capture.complete());
    capture.finish(Stream::Stdout, StreamEnd::Stopped);
    BOOST_CHECK(capture.view(Stream::Stdout)->end == StreamEnd::Eof);
    BOOST_TEST(!capture.view(Stream::Stdout)->truncated);
    const auto error = boost::system::errc::make_error_code(boost::system::errc::io_error);
    capture.finish(Stream::Stderr, StreamEnd::Failed, error);
    BOOST_TEST(capture.complete());
    BOOST_CHECK(capture.view(Stream::Stderr)->end == StreamEnd::Failed);
    BOOST_TEST(capture.view(Stream::Stderr)->error == error);
    BOOST_TEST(capture.view(Stream::Stderr)->truncated);
    BOOST_CHECK_THROW(capture.append(Stream::Stdout, "late"), std::logic_error);
    BOOST_CHECK_THROW(capture.finish(Stream::Stdout, StreamEnd::Open), std::invalid_argument);

    CaptureBuffer stopped(OutputTopology::Terminal, 0);
    stopped.finish(Stream::Terminal, StreamEnd::Stopped);
    BOOST_TEST(stopped.complete());
    BOOST_TEST(stopped.view(Stream::Terminal)->truncated);
}
