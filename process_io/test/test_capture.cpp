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

BOOST_AUTO_TEST_CASE(prefix_limit_counts_raw_bytes_across_streams)
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
    BOOST_TEST(capture.view(Stream::Stderr)->bytes.empty());
    BOOST_TEST(capture.view(Stream::Stderr)->truncated);
    capture.append(Stream::Stderr, "");
    BOOST_TEST(capture.view(Stream::Stderr)->truncated);
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

BOOST_AUTO_TEST_CASE(split_reads_share_capacity_and_only_dropped_streams_are_truncated)
{
    for (const bool reverse : {false, true}) {
        const auto first = reverse ? Stream::Stderr : Stream::Stdout;
        const auto second = reverse ? Stream::Stdout : Stream::Stderr;
        CaptureBuffer capture(OutputTopology::Separate, 8);
        capture.append(first, "12");
        capture.append(second, "abc");
        capture.append(first, "3456");
        BOOST_TEST(capture.view(first)->bytes == "12345");
        BOOST_TEST(capture.view(second)->bytes == "abc");
        BOOST_TEST(capture.view(first)->truncated);
        BOOST_TEST(!capture.view(second)->truncated);
        capture.append(second, "");
        BOOST_TEST(!capture.view(second)->truncated);
        capture.finish(first, StreamEnd::Eof);
        capture.append(second, "def"); // EOF does not refund retained capacity.
        BOOST_TEST(capture.view(second)->bytes == "abc");
        BOOST_TEST(capture.view(second)->truncated);
        capture.finish(second, StreamEnd::Eof);
        BOOST_TEST(capture.complete());
    }
}

BOOST_AUTO_TEST_CASE(exact_shared_boundary_and_empty_stream_do_not_imply_truncation)
{
    CaptureBuffer capture(OutputTopology::Separate, 8);
    capture.append(Stream::Stdout, "12345");
    capture.append(Stream::Stderr, "abc");
    capture.finish(Stream::Stdout, StreamEnd::Eof);
    capture.finish(Stream::Stderr, StreamEnd::Eof);
    BOOST_TEST(!capture.view(Stream::Stdout)->truncated);
    BOOST_TEST(!capture.view(Stream::Stderr)->truncated);
    BOOST_TEST(capture.complete());

    CaptureBuffer one(OutputTopology::Separate, 1);
    one.append(Stream::Stderr, "x");
    one.append(Stream::Stdout, "");
    one.finish(Stream::Stdout, StreamEnd::Eof);
    one.finish(Stream::Stderr, StreamEnd::Eof);
    BOOST_TEST(!one.view(Stream::Stdout)->truncated);
    BOOST_TEST(!one.view(Stream::Stderr)->truncated);
}

BOOST_AUTO_TEST_CASE(unlimited_pipes_and_terminal_budget_preserve_raw_prefixes)
{
    CaptureBuffer pipes(OutputTopology::Separate, 0);
    const std::string bytes(50000, 'x');
    pipes.append(Stream::Stdout, bytes);
    pipes.append(Stream::Stderr, bytes);
    pipes.append(Stream::Stdout, bytes);
    BOOST_TEST(pipes.view(Stream::Stdout)->bytes.size() == 100000U);
    BOOST_TEST(pipes.view(Stream::Stderr)->bytes.size() == 50000U);
    BOOST_TEST(!pipes.view(Stream::Stdout)->truncated);
    BOOST_TEST(!pipes.view(Stream::Stderr)->truncated);

    CaptureBuffer terminal(OutputTopology::Terminal, 2);
    terminal.append(Stream::Terminal, std::string_view("\xe4\xb8\xad", 3));
    BOOST_TEST(terminal.view(Stream::Terminal)->bytes == std::string_view("\xe4\xb8", 2));
    BOOST_TEST(terminal.view(Stream::Terminal)->truncated);
    BOOST_TEST(!terminal.view(Stream::Stdout));
    BOOST_TEST(!terminal.view(Stream::Stderr));
}
