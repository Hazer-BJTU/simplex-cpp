#define BOOST_TEST_MODULE LoggerTests
#include <boost/test/unit_test.hpp>

#include "logging/logger.hpp"

#include <algorithm>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace logging;

// ============================================================================
// Helpers — redirect std::cout / std::cerr into a stringstream
// ============================================================================

namespace {

/**
 * @brief RAII guard that redirects a std::ostream into a std::stringstream
 *        for the lifetime of the guard and restores the original buffer on
 *        destruction.
 */
class StreamRedirect {
public:
    StreamRedirect(std::ostream& target)
        : _target(target)
        , _original(target.rdbuf())
    {
        _target.rdbuf(_buffer.rdbuf());
    }

    ~StreamRedirect() {
        _target.rdbuf(_original);
    }

    std::string str() const { return _buffer.str(); }

private:
    std::ostream&     _target;
    std::streambuf*   _original;
    std::stringstream _buffer;
};

/**
 * @brief Check that `text` matches the expected log-line format:
 *
 *   [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] message\n
 *
 * Returns true when the regex matches.
 */
bool is_valid_log_line(const std::string& text,
                       const std::string& expected_level,
                       const std::string& expected_message)
{
    // Build a regex that anchors the expected level and message.
    // Timestamp: \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}
    std::string pattern =
        R"(^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\] \[)"
        + expected_level
        + R"(\] )"
        + expected_message
        + R"(\n$)";
    return std::regex_match(text, std::regex(pattern));
}

} // anonymous namespace

// ============================================================================
// Suite: LogLevelSuite — level filtering and configuration
// ============================================================================

BOOST_AUTO_TEST_SUITE(LogLevelSuite)

BOOST_AUTO_TEST_CASE(default_level_is_info_or_debug)
{
    // In test builds (typically Debug), level should be at most info.
    // Either debug or info is acceptable depending on compile flags.
    LogLevel lvl = Logger::level();
    BOOST_CHECK(lvl == LogLevel::info || lvl == LogLevel::debug);
}

BOOST_AUTO_TEST_CASE(set_and_get_level)
{
    Logger::set_level(LogLevel::warning);
    BOOST_CHECK_EQUAL(Logger::level(), LogLevel::warning);

    Logger::set_level(LogLevel::trace);
    BOOST_CHECK_EQUAL(Logger::level(), LogLevel::trace);

    // Restore sensible default
    Logger::set_level(LogLevel::info);
    BOOST_CHECK_EQUAL(Logger::level(), LogLevel::info);
}

BOOST_AUTO_TEST_CASE(messages_below_min_level_are_suppressed)
{
    Logger::set_level(LogLevel::error);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    // All of these should be suppressed
    Logger::trace("should not appear");
    Logger::debug("should not appear");
    Logger::info("should not appear");
    Logger::warning("should not appear");

    BOOST_CHECK(cout_guard.str().empty());
    BOOST_CHECK(cerr_guard.str().empty());

    // Restore
    Logger::set_level(LogLevel::info);
}

BOOST_AUTO_TEST_CASE(messages_at_or_above_min_level_are_emitted)
{
    Logger::set_level(LogLevel::warning);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    Logger::warning("visible warning");
    Logger::error("visible error");
    Logger::fatal("visible fatal");

    std::string out = cout_guard.str();
    std::string err = cerr_guard.str();

    BOOST_CHECK(out.find("visible warning") != std::string::npos);
    BOOST_CHECK(err.find("visible error")  != std::string::npos);
    BOOST_CHECK(err.find("visible fatal")  != std::string::npos);

    // Info should still be suppressed
    BOOST_CHECK(out.find("should not") == std::string::npos);

    Logger::set_level(LogLevel::info);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: FormatSuite — std::format-based logging
// ============================================================================

BOOST_AUTO_TEST_SUITE(FormatSuite)

BOOST_AUTO_TEST_CASE(simple_string_formatting)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    Logger::info("Hello, {}!", "world");

    std::string output = cout_guard.str();
    BOOST_CHECK(is_valid_log_line(output, "INFO  ", "Hello, world!"));
}

BOOST_AUTO_TEST_CASE(multiple_arguments)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    Logger::info("x={}, y={}, z={}", 10, 3.14, "hello");

    std::string output = cout_guard.str();
    BOOST_CHECK(is_valid_log_line(output, "INFO  ", "x=10, y=3.14, z=hello"));
}

BOOST_AUTO_TEST_CASE(integer_formatting)
{
    Logger::set_level(LogLevel::debug);

    StreamRedirect cout_guard(std::cout);
    Logger::debug("count={}, max={}", 42, 65535u);

    std::string output = cout_guard.str();
    BOOST_CHECK(is_valid_log_line(output, "DEBUG ", "count=42, max=65535"));
}

BOOST_AUTO_TEST_CASE(empty_format_string)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    Logger::info("");

    std::string output = cout_guard.str();
    BOOST_CHECK(is_valid_log_line(output, "INFO  ", ""));
}

BOOST_AUTO_TEST_CASE(format_with_special_characters)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    Logger::info("path='{}', line={}, col={}", "/home/user/file.txt", 100, 42);

    std::string output = cout_guard.str();
    BOOST_CHECK(is_valid_log_line(output, "INFO  ",
                                   "path='/home/user/file.txt', line=100, col=42"));
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: OutputRoutingSuite — stdout vs stderr
// ============================================================================

BOOST_AUTO_TEST_SUITE(OutputRoutingSuite)

BOOST_AUTO_TEST_CASE(info_goes_to_stdout)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    Logger::info("to stdout");

    BOOST_CHECK(!cout_guard.str().empty());
    BOOST_CHECK(cerr_guard.str().empty());
}

BOOST_AUTO_TEST_CASE(debug_goes_to_stdout)
{
    Logger::set_level(LogLevel::debug);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    Logger::debug("debug to stdout");

    BOOST_CHECK(!cout_guard.str().empty());
    BOOST_CHECK(cerr_guard.str().empty());
}

BOOST_AUTO_TEST_CASE(warning_goes_to_stdout)
{
    Logger::set_level(LogLevel::warning);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    Logger::warning("warning to stdout");

    BOOST_CHECK(!cout_guard.str().empty());
    BOOST_CHECK(cerr_guard.str().empty());
}

BOOST_AUTO_TEST_CASE(error_goes_to_stderr)
{
    Logger::set_level(LogLevel::error);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    Logger::error("to stderr");

    BOOST_CHECK(cout_guard.str().empty());
    BOOST_CHECK(!cerr_guard.str().empty());
}

BOOST_AUTO_TEST_CASE(fatal_goes_to_stderr)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    Logger::fatal("fatal to stderr");

    BOOST_CHECK(cout_guard.str().empty());
    BOOST_CHECK(!cerr_guard.str().empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: TimestampSuite — timestamp format verification
// ============================================================================

BOOST_AUTO_TEST_SUITE(TimestampSuite)

BOOST_AUTO_TEST_CASE(timestamp_has_correct_format)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    Logger::info("timestamp test");

    std::string output = cout_guard.str();

    // The timestamp should be: [YYYY-MM-DD HH:MM:SS.mmm]
    std::regex ts_regex(
        R"(\[(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{3})\])");
    std::smatch match;
    BOOST_CHECK_MESSAGE(std::regex_search(output, match, ts_regex),
                        "Timestamp not found in: " << output);

    // Validate ranges
    int year  = std::stoi(match[1].str());
    int month = std::stoi(match[2].str());
    int day   = std::stoi(match[3].str());
    int hour  = std::stoi(match[4].str());
    int min   = std::stoi(match[5].str());
    int sec   = std::stoi(match[6].str());
    int ms    = std::stoi(match[7].str());

    BOOST_CHECK(year >= 2026);
    BOOST_CHECK(month >= 1 && month <= 12);
    BOOST_CHECK(day   >= 1 && day   <= 31);
    BOOST_CHECK(hour  >= 0 && hour  <= 23);
    BOOST_CHECK(min   >= 0 && min   <= 59);
    BOOST_CHECK(sec   >= 0 && sec   <= 59);
    BOOST_CHECK(ms    >= 0 && ms    <= 999);
}

BOOST_AUTO_TEST_CASE(level_label_is_six_chars_padded)
{
    Logger::set_level(LogLevel::trace);

    {
        StreamRedirect cout_guard(std::cout);
        Logger::trace("t");
        // "TRACE " (6 chars, right-padded with space)
        BOOST_CHECK(cout_guard.str().find("[TRACE ]") != std::string::npos);
    }
    {
        StreamRedirect cout_guard(std::cout);
        Logger::debug("d");
        BOOST_CHECK(cout_guard.str().find("[DEBUG ]") != std::string::npos);
    }
    {
        StreamRedirect cout_guard(std::cout);
        Logger::info("i");
        BOOST_CHECK(cout_guard.str().find("[INFO  ]") != std::string::npos);
    }
    {
        StreamRedirect cout_guard(std::cout);
        Logger::warning("w");
        BOOST_CHECK(cout_guard.str().find("[WARN  ]") != std::string::npos);
    }
    {
        StreamRedirect cerr_guard(std::cerr);
        Logger::error("e");
        BOOST_CHECK(cerr_guard.str().find("[ERROR ]") != std::string::npos);
    }
    {
        StreamRedirect cerr_guard(std::cerr);
        Logger::fatal("f");
        BOOST_CHECK(cerr_guard.str().find("[FATAL ]") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: StringViewSuite — pre-formatted string overloads
// ============================================================================

BOOST_AUTO_TEST_SUITE(StringViewSuite)

BOOST_AUTO_TEST_CASE(string_view_overload_info)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    std::string msg = "pre-formatted info message";
    Logger::info(std::string_view(msg));

    std::string output = cout_guard.str();
    BOOST_CHECK(is_valid_log_line(output, "INFO  ", "pre-formatted info message"));
}

BOOST_AUTO_TEST_CASE(string_view_overload_error)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cerr_guard(std::cerr);
    Logger::error(std::string_view("pre-formatted error"));

    std::string output = cerr_guard.str();
    BOOST_CHECK(is_valid_log_line(output, "ERROR ", "pre-formatted error"));
}

BOOST_AUTO_TEST_CASE(string_view_overload_all_levels)
{
    Logger::set_level(LogLevel::trace);

    // Just ensure they compile and don't crash
    Logger::trace("sv trace");
    Logger::debug("sv debug");
    Logger::info("sv info");
    Logger::warning("sv warning");
    Logger::error("sv error");
    Logger::fatal("sv fatal");

    BOOST_CHECK(true); // reached without crashing
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: ThreadSafetySuite — concurrent writes from multiple threads
// ============================================================================

BOOST_AUTO_TEST_SUITE(ThreadSafetySuite)

BOOST_AUTO_TEST_CASE(concurrent_writes_do_not_crash_or_interleave_lines)
{
    Logger::set_level(LogLevel::info);

    const int num_threads = 16;
    const int msgs_per_thread = 500;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([t, msgs_per_thread]() {
            for (int i = 0; i < msgs_per_thread; ++i) {
                Logger::info("thread={}, msg={}", t, i);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    std::string output = cout_guard.str();

    // Every line must start with a valid timestamp (i.e. no interleaving)
    std::regex line_start(R"(^\[\d{4}-\d{2}-\d{2})");
    int line_count = 0;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        ++line_count;
        BOOST_CHECK_MESSAGE(std::regex_search(line, line_start),
                            "Line does not start with timestamp: " << line);
    }

    // We expect num_threads * msgs_per_thread lines (each log call produces one line)
    BOOST_CHECK_EQUAL(line_count, num_threads * msgs_per_thread);
}

BOOST_AUTO_TEST_CASE(concurrent_mixed_levels_dont_interleave)
{
    Logger::set_level(LogLevel::trace);

    const int num_threads = 8;
    const int msgs_per_thread = 200;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([t, msgs_per_thread]() {
            for (int i = 0; i < msgs_per_thread; ++i) {
                // Mix levels
                switch (i % 4) {
                    case 0: Logger::info("t={}, i={}", t, i); break;
                    case 1: Logger::debug("t={}, i={}", t, i); break;
                    case 2: Logger::warning("t={}, i={}", t, i); break;
                    case 3: Logger::error("t={}, i={}", t, i); break;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // No crash = success.  Lines may be in any order (threads interleave by
    // design) but each individual line must be complete.
    std::string out = cout_guard.str();
    std::string err = cerr_guard.str();

    std::regex line_start(R"(^\[\d{4}-\d{2}-\d{2})");

    {
        std::istringstream stream(out);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            BOOST_CHECK_MESSAGE(std::regex_search(line, line_start),
                                "stdout line interleaved: " << line);
        }
    }
    {
        std::istringstream stream(err);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            BOOST_CHECK_MESSAGE(std::regex_search(line, line_start),
                                "stderr line interleaved: " << line);
        }
    }
}

BOOST_AUTO_TEST_CASE(set_level_is_thread_safe)
{
    // Toggle the log level from multiple threads while other threads are
    // logging.  The only invariant we can check is: no crash / no data race.
    const int num_writers = 8;
    const int num_togglers = 4;

    std::vector<std::thread> threads;
    threads.reserve(num_writers + num_togglers);

    std::atomic<bool> start{false};

    for (int t = 0; t < num_writers; ++t) {
        threads.emplace_back([&start, t]() {
            while (!start.load()) { /* spin */ }
            for (int i = 0; i < 200; ++i) {
                Logger::info("writer={}, msg={}", t, i);
                Logger::error("writer={}, err={}", t, i);
            }
        });
    }

    for (int t = 0; t < num_togglers; ++t) {
        threads.emplace_back([&start]() {
            while (!start.load()) { /* spin */ }
            const LogLevel levels[] = {
                LogLevel::trace, LogLevel::debug, LogLevel::info,
                LogLevel::warning, LogLevel::error, LogLevel::fatal
            };
            for (int i = 0; i < 100; ++i) {
                Logger::set_level(levels[i % 6]);
            }
        });
    }

    start.store(true);

    for (auto& th : threads) {
        th.join();
    }

    // Restore
    Logger::set_level(LogLevel::info);
    BOOST_CHECK(true); // no crash
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: EdgeCaseSuite — boundary and edge-case behaviours
// ============================================================================

BOOST_AUTO_TEST_SUITE(EdgeCaseSuite)

BOOST_AUTO_TEST_CASE(very_long_message)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    std::string long_msg(10000, 'x');
    Logger::info("{}", long_msg);

    std::string output = cout_guard.str();
    BOOST_CHECK(output.find(long_msg) != std::string::npos);
    // Should still have valid format
    BOOST_CHECK(output.find("[INFO  ]") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(unicode_message)
{
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    Logger::info("Unicode: 你好, 世界! — café π≈3.14");

    std::string output = cout_guard.str();
    BOOST_CHECK(output.find("你好, 世界!") != std::string::npos);
    BOOST_CHECK(output.find("café") != std::string::npos);
    BOOST_CHECK(output.find("π") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(braces_in_message)
{
    // Literal braces in a format string are escaped as {{ and }}.
    Logger::set_level(LogLevel::info);

    StreamRedirect cout_guard(std::cout);
    Logger::info("set={{x={}, y={}}}", 1, 2);

    std::string output = cout_guard.str();
    BOOST_CHECK(output.find("set={x=1, y=2}") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(trace_level_when_enabled)
{
    Logger::set_level(LogLevel::trace);

    StreamRedirect cout_guard(std::cout);
    Logger::trace("finest grain: {}", 42);

    std::string output = cout_guard.str();
    BOOST_CHECK(output.find("[TRACE ]") != std::string::npos);
    BOOST_CHECK(output.find("finest grain: 42") != std::string::npos);

    Logger::set_level(LogLevel::info);
}

BOOST_AUTO_TEST_CASE(fatal_level_always_visible_when_enabled)
{
    Logger::set_level(LogLevel::fatal);

    StreamRedirect cout_guard(std::cout);
    StreamRedirect cerr_guard(std::cerr);

    Logger::info("should not appear");
    Logger::error("should not appear either");
    Logger::fatal("FATAL shows up");

    BOOST_CHECK(cout_guard.str().empty());
    std::string err = cerr_guard.str();
    BOOST_CHECK(err.find("FATAL shows up") != std::string::npos);
    BOOST_CHECK(err.find("[FATAL ]") != std::string::npos);

    Logger::set_level(LogLevel::info);
}

BOOST_AUTO_TEST_SUITE_END()
