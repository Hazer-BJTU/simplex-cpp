#define BOOST_TEST_MODULE ProcessHandleTests
#include <boost/test/unit_test.hpp>

#include "scenario.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>

#include <boost/asio.hpp>

// Lifecycle tests for ProcessHandle against real but harmless children:
// echo / false / cat / env / sleep / seq — coreutils any POSIX system has,
// nothing that touches files outside a temp-dir fixture or the network
// (the PATH-resolution case builds its tiny fixture under
// temp_directory_path and removes it again). Heavier fixtures (leak
// audits, destruction storms, real toolchain invocations) live in
// test_process_destructive and belong to disposable environments.

using process_test::run_scenario;

BOOST_AUTO_TEST_CASE(spawns_echo_and_captures_stdout)
{
    auto s = run_scenario(process::LaunchSpec{
        .executable = "echo",
        .arguments = {"hello", "from", "child"},
        .description = "echo a line",
        .initial_wait_timeout_milliseconds = std::uint64_t{5000},
    });

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->exited());

    const process::ExecutionStatus status = s.handle->status();
    BOOST_CHECK(status.state == process::ProcessState::Exited);
    BOOST_TEST(status.exit_code.value() == 0);
    BOOST_TEST(s.handle->standard_output() == "hello from child\n");
    BOOST_TEST(s.handle->standard_error().empty());
    // Well under the default cap: neither capture is partial.
    BOOST_TEST(!s.handle->stdout_truncated());
    BOOST_TEST(!s.handle->stderr_truncated());
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
        .initial_wait_timeout_milliseconds = std::uint64_t{5000},
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
            .initial_wait_timeout_milliseconds = std::uint64_t{5000},
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
        .initial_wait_timeout_milliseconds = std::uint64_t{5000},
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

BOOST_AUTO_TEST_CASE(overrides_an_inherited_environment_entry_by_key)
{
    // execve entries with duplicated names are undefined — glibc's getenv
    // returns the FIRST match, so appending "FOO=new" after an inherited
    // "FOO=old" would hand the child "old". The constructor therefore
    // merges by key: the explicit entry fully replaces the inherited one,
    // and the child sees exactly one FOO.
    ::setenv("SIMPLEX_PROCESS_TEST_OVERRIDE", "inherited-value", 1);
    auto s = run_scenario(process::LaunchSpec{
        .executable = "env",
        .arguments = {},
        .description = "print the merged environment",
        .initial_wait_timeout_milliseconds = std::uint64_t{5000},
        .environment = std::vector<std::string>{
            "SIMPLEX_PROCESS_TEST_OVERRIDE=explicit-value"},
    });

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->status().exit_code.value() == 0);
    BOOST_TEST(s.handle->standard_output().find(
                   "SIMPLEX_PROCESS_TEST_OVERRIDE=explicit-value") !=
               std::string::npos);
    BOOST_TEST(s.handle->standard_output().find(
                   "SIMPLEX_PROCESS_TEST_OVERRIDE=inherited-value") ==
               std::string::npos);
}

BOOST_AUTO_TEST_CASE(resolves_the_executable_through_the_spec_supplied_path)
{
    // inherit_environment=false plus an explicit PATH: the executable is
    // resolved inside the directory the spec supplies (the fixture exists
    // nowhere else), and the parent's other variables never cross into
    // the child — its environment is exactly what the spec assembled. The
    // fixture reports through shell builtins only: with the child's PATH
    // pointing at the fixture directory alone, no external command (env,
    // printenv) would resolve anyway — which is rather the point.
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
        ("simplex-process-test-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const auto tool = dir / "simplex-fixture-tool";
    {
        std::ofstream file{tool};
        // ${VAR+yes} is the POSIX "is set" expansion: "yes" when VAR is
        // set (even empty), nothing when it is unset.
        file << "#!/bin/sh\n"
                "echo resolved-via-spec-path\n"
                "echo spec-marker=$SIMPLEX_PROCESS_TEST_SPEC_MARKER\n"
                "echo parent-marker=$SIMPLEX_PROCESS_TEST_PARENT_ONLY\n"
                "echo path-is-set=${PATH+yes}\n";
    }
    fs::permissions(tool, fs::perms::owner_exec, fs::perm_options::add);

    ::setenv("SIMPLEX_PROCESS_TEST_PARENT_ONLY", "must-not-inherit", 1);
    auto s = run_scenario(process::LaunchSpec{
        .executable = "simplex-fixture-tool",
        .arguments = {},
        .description = "resolved through the spec's PATH",
        .initial_wait_timeout_milliseconds = std::uint64_t{5000},
        .environment = std::vector<std::string>{
            std::format("PATH={}", dir.string()),
            "SIMPLEX_PROCESS_TEST_SPEC_MARKER=present",
        },
        .inherit_environment = false,
    });
    std::error_code cleanup_ec;
    fs::remove_all(dir, cleanup_ec);

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->status().exit_code.value() == 0);
    // The fixture ran at all => the name resolved through the spec's PATH.
    BOOST_TEST(s.handle->standard_output().find(
                   "resolved-via-spec-path") != std::string::npos);
    BOOST_TEST(s.handle->standard_output().find(
                   "spec-marker=present\n") != std::string::npos);
    // No inheritance: the parent-only marker crossed as empty, and the
    // only PATH in the child is the spec's.
    BOOST_TEST(s.handle->standard_output().find(
                   "parent-marker=\n") != std::string::npos);
    BOOST_TEST(s.handle->standard_output().find(
                   "must-not-inherit") == std::string::npos);
    BOOST_TEST(s.handle->standard_output().find(
                   "path-is-set=yes\n") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(resolves_bare_names_via_parent_path_when_child_env_has_none)
{
    // The documented fallback: with inherit_environment=false and no PATH
    // entry at all, the child's environment really has no PATH — but the
    // executable still resolves, against the PARENT's PATH (lookup only;
    // the child's environment is unaffected by the fallback).
    ::setenv("SIMPLEX_PROCESS_TEST_PARENT_ONLY", "must-not-inherit", 1);
    auto s = run_scenario(process::LaunchSpec{
        .executable = "env",
        .arguments = {},
        .description = "no PATH anywhere in the spec",
        .initial_wait_timeout_milliseconds = std::uint64_t{5000},
        .environment = std::vector<std::string>{
            "SIMPLEX_PROCESS_TEST_SPEC_MARKER=present"},
        .inherit_environment = false,
    });

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->status().exit_code.value() == 0);
    // env resolved through the parent PATH fallback and ran; its
    // environment is only the spec's entry — no PATH line, no parent
    // marker.
    BOOST_TEST(s.handle->standard_output() ==
               "SIMPLEX_PROCESS_TEST_SPEC_MARKER=present\n");
}

BOOST_AUTO_TEST_CASE(malformed_environment_entry_throws_at_environment_stage)
{
    // A malformed KEY=VALUE entry is a launch failure the caller can act
    // on — Stage::Environment — not a surprise inside the child's environ.
    bool threw = false;
    try {
        boost::asio::io_context io;
        auto strand = boost::asio::make_strand(io);
        auto handle = std::make_shared<process::ProcessHandle>(
            process::LaunchSpec{
                .executable = "echo",
                .arguments = {"never", "launched"},
                .description = "doomed launch",
                .initial_wait_timeout_milliseconds = std::uint64_t{1000},
                .environment = std::vector<std::string>{"NO_EQUALS_SIGN"},
            },
            strand);
        (void)handle;
    } catch (const process::ProcessException& e) {
        threw = true;
        BOOST_CHECK(e.stage() ==
                    process::ProcessException::Stage::Environment);
        // what() carries the offending entry alongside the launch context.
        BOOST_TEST(std::string(e.what()).find("NO_EQUALS_SIGN") !=
                   std::string::npos);
    }
    BOOST_TEST(threw);
}

BOOST_AUTO_TEST_CASE(truncates_output_at_the_configured_cap)
{
    // seq 1 8192 emits ~39 KB — far past a 4096-byte cap. Exactly the
    // first 4096 bytes are kept; the reader KEEPS DRAINING past the cap
    // (so the child never blocks on a full pipe and still exits 0), and
    // the truncated flag reports that the capture is partial. stderr said
    // nothing, so its flag stays down.
    auto s = run_scenario(process::LaunchSpec{
        .executable = "seq",
        .arguments = {"1", "8192"},
        .description = "emit more than the cap",
        .initial_wait_timeout_milliseconds = std::uint64_t{5000},
        .max_output_bytes = std::uint64_t{4096},
    });

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->status().exit_code.value() == 0);
    BOOST_TEST(s.handle->standard_output().size() == std::size_t{4096});
    BOOST_TEST(s.handle->stdout_truncated());
    BOOST_TEST(!s.handle->stderr_truncated());

    // snapshot() carries the flags so reports can say "partial capture".
    const process::ExecutionResult result = s.handle->snapshot();
    BOOST_TEST(result.stdout_truncated);
    BOOST_TEST(!result.stderr_truncated);
    BOOST_TEST(result.stdout_text->size() == std::size_t{4096});
}

BOOST_AUTO_TEST_CASE(zero_output_cap_captures_everything)
{
    // max_output_bytes == 0 disables the cap: a chatty child is captured
    // whole and neither flag ever goes up.
    auto s = run_scenario(process::LaunchSpec{
        .executable = "seq",
        .arguments = {"1", "100"},
        .description = "emit a bounded burst",
        .initial_wait_timeout_milliseconds = std::uint64_t{5000},
        .max_output_bytes = std::uint64_t{0},
    });

    BOOST_TEST(s.finished_on_time);
    BOOST_TEST(s.handle->status().exit_code.value() == 0);
    BOOST_TEST(s.handle->standard_output().find("100\n") != std::string::npos);
    BOOST_TEST(!s.handle->stdout_truncated());
    BOOST_TEST(!s.handle->stderr_truncated());
}

BOOST_AUTO_TEST_CASE(timeout_with_kill_terminates_the_child)
{
    auto s = run_scenario(
        process::LaunchSpec{
            .executable = "sleep",
            .arguments = {"30"},
            .description = "outlive the deadline",
            .initial_wait_timeout_milliseconds = std::uint64_t{200},
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
        .initial_wait_timeout_milliseconds = std::uint64_t{150},
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
    // initial_wait_timeout_milliseconds == 0 disables the deadline: a
    // child outliving any nonzero timer still reports finished_on_time ==
    // true.
    auto s = run_scenario(process::LaunchSpec{
        .executable = "sleep",
        .arguments = {"0.3"},
        .description = "no deadline applies",
        .initial_wait_timeout_milliseconds = std::uint64_t{0},
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
            .initial_wait_timeout_milliseconds = std::uint64_t{0},
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
                .initial_wait_timeout_milliseconds = std::uint64_t{1000},
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
