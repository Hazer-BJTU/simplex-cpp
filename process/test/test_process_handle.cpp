#define BOOST_TEST_MODULE ProcessHandleTests
#include <boost/test/unit_test.hpp>

#include "process/process_handle.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <cerrno>
#include <csignal>
#include <sys/types.h>

#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>

// Lifecycle tests for ProcessHandle against real but side-effect-free
// children: echo / false / cat / env / sleep — coreutils any POSIX system
// has, nothing that writes files or touches the network. Each case drives
// one handle on a private io_context + strand (the usage pattern the class
// comment documents) and runs the io_context to quiescence before
// asserting, so what the assertions see is the settled aftermath: final
// status recorded, pipes drained, stdin channel closed. Heavier fixtures
// (real toolchain invocations) belong to the container build later.

namespace {

// One test run: owns the io_context and the handle, in that order, so the
// handle (and its asio objects) is destroyed before its io_context —
// declaration order is destruction order here.
struct Scenario {
    std::unique_ptr<boost::asio::io_context> io;
    std::shared_ptr<process::ProcessHandle> handle;
    bool finished_on_time = false;
    std::chrono::system_clock::time_point before_spawn;
    std::chrono::system_clock::time_point after_spawn;
};

// Spawns the spec, runs the standard lifecycle (start tasks -> optional
// mid-life callback while the child is guaranteed alive -> await against the
// deadline) and waits for full quiescence. The callback is where a case
// feeds stdin or observes the live (Running) view.
Scenario run_scenario(
    process::LaunchSpec spec,
    std::function<void(process::ProcessHandle&)> on_running = {})
{
    Scenario s;
    s.io = std::make_unique<boost::asio::io_context>();
    auto strand = boost::asio::make_strand(*s.io);

    s.before_spawn = std::chrono::system_clock::now();
    s.handle = std::make_shared<process::ProcessHandle>(std::move(spec), strand);
    s.after_spawn = std::chrono::system_clock::now();

    auto done = boost::asio::co_spawn(
        strand,
        [handle = s.handle, &finished = s.finished_on_time, on_running]()
            -> boost::asio::awaitable<void> {
            co_await handle->start_background_io_tasks();
            if (on_running) on_running(*handle);
            finished = co_await handle->await_initial_execution();
        },
        boost::asio::use_future);

    s.io->run();
    done.get();
    return s;
}

} // namespace

BOOST_AUTO_TEST_CASE(spawns_echo_and_captures_stdout)
{
    auto s = run_scenario(process::LaunchSpec{
        .executable = "echo",
        .arguments = {"hello", "from", "child"},
        .description = "echo a line",
        .timeout_milliseconds = std::uint64_t{5000},
    });

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->exited());

    const process::ExecutionStatus status = s.handle->status();
    BOOST_CHECK(status.state == process::ProcessState::Exited);
    BOOST_TEST(status.exit_code.value() == 0);
    BOOST_TEST(s.handle->standard_output() == "hello from child\n");
    BOOST_TEST(s.handle->standard_error().empty());
    BOOST_TEST(s.handle->pid() > 0);

    // The stamps land inside the constructor's time window.
    BOOST_CHECK(s.handle->started_at() >= s.before_spawn);
    BOOST_CHECK(s.handle->started_at() <= s.after_spawn);

    // snapshot() is the report-shaped view: identity, status, both streams
    // ("" is captured silence, never absent).
    const process::ExecutionResult result = s.handle->snapshot();
    BOOST_TEST(result.spec.executable == "echo");
    BOOST_TEST(result.spec.pid == s.handle->pid());
    BOOST_CHECK(result.spec.started_at == s.handle->started_at());
    BOOST_CHECK(result.execution.state == process::ProcessState::Exited);
    BOOST_TEST(result.execution.exit_code.value() == 0);
    BOOST_TEST(result.stdout_text.value() == "hello from child\n");
    BOOST_TEST(result.stderr_text.has_value());
    BOOST_TEST(result.stderr_text->empty());
}

BOOST_AUTO_TEST_CASE(propagates_a_nonzero_exit_code)
{
    auto s = run_scenario(process::LaunchSpec{
        .executable = "false",
        .arguments = {},
        .description = "exit 1",
        .timeout_milliseconds = std::uint64_t{5000},
    });

    BOOST_TEST(s.finished_on_time);
    const process::ExecutionStatus status = s.handle->status();
    BOOST_CHECK(status.state == process::ProcessState::Exited);
    BOOST_TEST(status.exit_code.value() == 1);
    BOOST_TEST(s.handle->standard_output().empty());
}

BOOST_AUTO_TEST_CASE(feeds_stdin_and_reads_it_back)
{
    auto s = run_scenario(
        process::LaunchSpec{
            .executable = "cat",
            .arguments = {},
            .description = "echo stdin back",
            .timeout_milliseconds = std::uint64_t{5000},
        },
        [](process::ProcessHandle& handle) {
            // Two writes then close: proves the channel keeps order AND that
            // a closed channel still delivers what was buffered (the child
            // must see both lines before its stdin EOF).
            handle.write_input("line one\n");
            handle.write_input("line two\n");
            handle.close_input();
        });

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->status().exit_code.value() == 0);
    BOOST_TEST(s.handle->standard_output() == "line one\nline two\n");
}

BOOST_AUTO_TEST_CASE(passes_environment_entries_to_the_child)
{
    auto s = run_scenario(process::LaunchSpec{
        .executable = "env",
        .arguments = {},
        .description = "print the child environment",
        .timeout_milliseconds = std::uint64_t{5000},
        .environment = std::vector<std::string>{
            "SIMPLEX_PROCESS_TEST_MARKER=sentinel-value"},
    });

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->status().exit_code.value() == 0);
    // The spec's KEY=VALUE entry reached the child, and the inherited
    // environment did too (inherit_environment defaults to true).
    BOOST_TEST(s.handle->standard_output().find(
                   "SIMPLEX_PROCESS_TEST_MARKER=sentinel-value") !=
               std::string::npos);
    BOOST_TEST(s.handle->standard_output().find("PATH=") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(timeout_with_kill_terminates_the_child)
{
    auto s = run_scenario(
        process::LaunchSpec{
            .executable = "sleep",
            .arguments = {"30"},
            .description = "outlive the deadline",
            .timeout_milliseconds = std::uint64_t{200},
            .detach_on_timeout = false,
        },
        [](process::ProcessHandle& handle) {
            // Mid-life, before any deadline verdict: the live view is
            // Running, not Unknown.
            BOOST_CHECK(handle.status().state ==
                        process::ProcessState::Running);
        });

    BOOST_TEST(!s.finished_on_time);
    // The restarted await task observed the kill: v2's terminate() is a
    // hard kill, and posix evaluate_exit_code reports a signal death as
    // the positive signal number (SIGKILL == 9).
    BOOST_TEST(s.handle->exited());
    const process::ExecutionStatus status = s.handle->status();
    BOOST_CHECK(status.state == process::ProcessState::Exited);
    BOOST_TEST(status.exit_code.value() == 9);
    BOOST_TEST(status.cumulative_execution_milliseconds >=
               std::uint64_t{150});
}

BOOST_AUTO_TEST_CASE(timeout_with_detach_records_the_eventual_exit)
{
    auto s = run_scenario(process::LaunchSpec{
        .executable = "sleep",
        .arguments = {"1"},
        .description = "outlive the deadline, die naturally",
        .timeout_milliseconds = std::uint64_t{150},
        // detach_on_timeout defaults to true: the child is left running and
        // the restarted await task records its natural exit.
    });

    BOOST_TEST(!s.finished_on_time);
    BOOST_TEST(s.handle->exited());
    const process::ExecutionStatus status = s.handle->status();
    BOOST_CHECK(status.state == process::ProcessState::Exited);
    BOOST_TEST(status.exit_code.value() == 0);
    // Detached but still tracked: the sleep really ran out its second.
    BOOST_TEST(status.cumulative_execution_milliseconds >=
               std::uint64_t{900});
}

BOOST_AUTO_TEST_CASE(zero_timeout_waits_indefinitely)
{
    // timeout_milliseconds == 0 disables the deadline: a child outliving
    // any nonzero timer still reports finished_on_time == true.
    auto s = run_scenario(process::LaunchSpec{
        .executable = "sleep",
        .arguments = {"0.3"},
        .description = "no deadline applies",
        .timeout_milliseconds = std::uint64_t{0},
    });

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->exited());
    BOOST_TEST(s.handle->status().exit_code.value() == 0);
}

BOOST_AUTO_TEST_CASE(destruction_terminates_a_running_child)
{
    // Detach is a within-lifetime concept: destroying a handle whose child
    // is still alive force-kills it (SIGKILL + reap) instead of leaking it.
    boost::asio::io_context io;
    auto strand = boost::asio::make_strand(io);
    auto handle = std::make_shared<process::ProcessHandle>(
        process::LaunchSpec{
            .executable = "sleep",
            .arguments = {"30"},
            .description = "outlives its handle",
            .timeout_milliseconds = std::uint64_t{0},
        },
        strand);
    const pid_t pid = handle->pid();
    BOOST_TEST(pid > 0);

    handle.reset(); // destructor with the child mid-flight

    // SIGKILLed and reaped: the pid no longer exists for kill(2).
    const int probe = ::kill(pid, 0);
    BOOST_TEST(probe == -1);
    BOOST_TEST(errno == ESRCH);
}

BOOST_AUTO_TEST_CASE(unresolvable_executable_throws_at_resolve_stage)
{
    bool threw = false;
    try {
        boost::asio::io_context io;
        auto strand = boost::asio::make_strand(io);
        auto handle = std::make_shared<process::ProcessHandle>(
            process::LaunchSpec{
                .executable = "simplex-no-such-executable-xyz",
                .arguments = {},
                .description = "doomed launch",
                .timeout_milliseconds = std::uint64_t{1000},
            },
            strand);
        (void)handle;
    } catch (const process::ProcessException& e) {
        threw = true;
        BOOST_CHECK(e.stage() ==
                    process::ProcessException::Stage::ResolveExecutable);
        // what() carries the launch context — the executable name at least.
        BOOST_TEST(std::string(e.what()).find(
                       "simplex-no-such-executable-xyz") !=
                   std::string::npos);
    }
    BOOST_TEST(threw);
}
