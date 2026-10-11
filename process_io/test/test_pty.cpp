#define BOOST_TEST_MODULE ProcessIoPty
#include <boost/test/unit_test.hpp>
#include "harness.hpp"
#include "process_io/posix_pty.hpp"
#include <filesystem>
#include <fstream>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <climits>
#include <boost/scope/scope_exit.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/post.hpp>
#include <fcntl.h>
#include <unistd.h>

using namespace process_io;
namespace {
struct PtyHarness : Harness {
    explicit PtyHarness(std::size_t cap = 4 * 1024 * 1024) {
        environment = std::make_unique<PosixPtyIo>(strand, cap);
    }
    void launch_pty(const std::string& mode, const std::string& term = "") {
        auto attachment = environment->prepare_attachment();
        process::LaunchSpec spec;
        spec.executable = PROCESS_PTY_CHILD;
        spec.arguments = {mode};
        if (!term.empty()) {
            spec.inherit_environment = false;
            spec.environment = std::vector<std::string>{"TERM=" + term};
        }
        child = std::make_unique<subprocess::Child>(spec, strand, attachment);
    }
};
asio::awaitable<void> ready(Harness& h)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!h.environment->snapshot().terminal_output->bytes.starts_with("ready")) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("PTY not ready");
        co_await h.pause();
    }
}
asio::awaitable<void> drained(Harness& h)
{
    BOOST_TEST(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 5s));
    BOOST_TEST(co_await h.wait_exit() == 0);
}
std::size_t descriptors()
{
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
}
}
BOOST_AUTO_TEST_CASE(real_controlling_terminal_and_host_defaults)
{
    const auto session = ::getsid(0);
    const auto group = ::getpgrp();
    const std::string parent_term = ::getenv("TERM") ? ::getenv("TERM") : "";
    for (const std::string term : {"", "xterm-test"}) {
        PtyHarness h;
        h.launch_pty("probe", term);
        h.run([term](Harness& h) -> asio::awaitable<void> {
            BOOST_TEST(h.environment->capabilities().is_terminal);
            co_await h.environment->start();
            co_await drained(h);
            const auto snapshot = h.environment->snapshot();
            BOOST_TEST(!snapshot.standard_output);
            BOOST_TEST(!snapshot.standard_error);
            BOOST_TEST(snapshot.terminal_output->bytes == "probe-ok:" + (term.empty() ? "dumb" : term));
            BOOST_CHECK(snapshot.terminal_output->end == StreamEnd::Eof);
        }, 3);
    }
    BOOST_TEST(::getsid(0) == session);
    BOOST_TEST(::getpgrp() == group);
    BOOST_TEST(std::string(::getenv("TERM") ? ::getenv("TERM") : "") == parent_term);
}
BOOST_AUTO_TEST_CASE(queued_eof_flushes_unterminated_line_and_preserves_tail)
{
    for (const std::string mode : {"echo", "custom-eof"}) {
        PtyHarness h;
        h.launch_pty(mode);
        h.run([](Harness& h) -> asio::awaitable<void> {
            co_await h.environment->start();
            co_await ready(h);
            BOOST_CHECK((co_await h.environment->enqueue_input("first\n")) == InputResult::Accepted);
            BOOST_CHECK((co_await h.environment->enqueue_input("last")) == InputResult::Accepted);
            BOOST_CHECK((co_await h.environment->request_input_end()) == InputResult::Accepted);
            BOOST_CHECK((co_await h.environment->enqueue_input("late")) == InputResult::Closed);
            co_await drained(h);
            BOOST_TEST(h.environment->snapshot().terminal_output->bytes ==
                "readyfirst\r\nlastout-tail\r\nerr-tail\r\n");
            BOOST_TEST(h.environment->snapshot().input_end_requested);
            const auto phase = h.environment->snapshot().input_phase;
            BOOST_CHECK(phase == InputPhase::Ended || phase == InputPhase::PeerClosed);
        });
    }
}
BOOST_AUTO_TEST_CASE(actual_raw_or_disabled_eof_is_visible_without_hangup)
{
    for (const std::string mode : {"raw", "disabled-eof", "signal-eof"}) {
        PtyHarness h;
        h.launch_pty(mode);
        h.run([](Harness& h) -> asio::awaitable<void> {
            co_await h.environment->start();
            co_await ready(h);
            co_await h.environment->request_input_end();
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (h.environment->snapshot().input_phase == InputPhase::EndRequested
                   && std::chrono::steady_clock::now() < deadline) co_await h.pause();
            BOOST_CHECK(h.environment->snapshot().input_phase == InputPhase::Unsupported);
            BOOST_TEST(!h.environment->snapshot().output_complete);
            boost::system::error_code error;
            BOOST_TEST(h.child->running(error));
            BOOST_TEST(!error);
        });
    }
}
BOOST_AUTO_TEST_CASE(host_resize_and_foreground_winch)
{
    PtyHarness h;
    h.launch_pty("resize");
    h.run([](Harness& h) -> asio::awaitable<void> {
        co_await h.environment->start();
        co_await ready(h);
        for (const auto dimensions : {std::pair{0u, 24u}, std::pair{80u, 65536u}}) {
            bool rejected = false;
            try { co_await h.environment->resize(dimensions.first, dimensions.second); }
            catch (const std::invalid_argument&) { rejected = true; }
            BOOST_TEST(rejected);
        }
        BOOST_CHECK((co_await h.environment->resize(120, 40)) == ResizeResult::Applied);
        co_await drained(h);
        BOOST_TEST(h.environment->snapshot().terminal_output->bytes == "ready120x40");
    });
}
BOOST_AUTO_TEST_CASE(shared_budget_keeps_draining_merged_output)
{
    PtyHarness h(37);
    h.launch_pty("burst");
    h.run([](Harness& h) -> asio::awaitable<void> {
        co_await h.environment->start();
        co_await drained(h);
        const auto view = *h.environment->snapshot().terminal_output;
        BOOST_TEST(view.bytes == "ready" + std::string(32, 'x'));
        BOOST_TEST(view.truncated);
        BOOST_CHECK(view.end == StreamEnd::Eof);
    });
}
BOOST_AUTO_TEST_CASE(blocked_input_can_be_cancelled_without_closing_output)
{
    PtyHarness h;
    h.launch_pty("no-read");
    h.run([](Harness& h) -> asio::awaitable<void> {
        co_await h.environment->start();
        co_await ready(h);
        std::string input;
        for (int i = 0; i < 32768; ++i) input += std::string(63, 'x') + "\n";
        co_await h.environment->enqueue_input(std::move(input));
        co_await h.pause(20ms);
        co_await h.environment->cancel_input();
        BOOST_TEST(!h.environment->snapshot().output_complete);
        BOOST_CHECK(h.environment->snapshot().input_phase == InputPhase::Stopped);
        BOOST_TEST(!(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 20ms)));
        co_await h.environment->cancel_and_join();
        co_await h.environment->cancel_and_join();
        BOOST_TEST(h.environment->snapshot().joined);
        BOOST_CHECK(h.environment->snapshot().terminal_output->end == StreamEnd::Stopped);
    }, 3);
}
BOOST_AUTO_TEST_CASE(failed_exec_closes_slave_reaps_and_does_not_leak)
{
    const auto before = descriptors();
    for (int i = 0; i < 24; ++i) {
        PtyHarness h;
        const auto attachment = h.environment->prepare_attachment();
        process::LaunchSpec spec;
        spec.executable = "/"; // exists but cannot exec; exercises child error channel
        bool failed = false;
        try { subprocess::Child child(spec, h.strand, attachment); }
        catch (const subprocess::ProcessException& error) {
            failed = true;
            BOOST_TEST(error.error_code().value() == EACCES);
            BOOST_TEST(std::string(error.what()).find("PTY execve") != std::string::npos);
        }
        BOOST_TEST(failed);
        h.run([](Harness& h) -> asio::awaitable<void> {
            bool rejected = false;
            try { co_await h.environment->start(); }
            catch (const std::logic_error&) { rejected = true; }
            BOOST_TEST(rejected);
        });
    }
    BOOST_TEST(descriptors() == before);
    int status;
    BOOST_TEST(::waitpid(-1, &status, WNOHANG) == -1);
    BOOST_TEST(errno == ECHILD);
}
BOOST_AUTO_TEST_CASE(closed_standard_descriptors_do_not_break_launch_or_errors)
{
    // Isolate descriptor manipulation from the test runner and its reporters.
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        try {
            PtyHarness h;
            // Allocate the reactor and terminal FIRST, then close stdio. This
            // forces the launch error pipe itself to obtain low descriptors;
            // closing before context construction would let reactor fds mask
            // the collision we need to exercise.
            ::close(0); ::close(1); ::close(2);
            h.launch_pty("probe");
            bool ok = false;
            h.run([&ok](Harness& h) -> asio::awaitable<void> {
                co_await h.environment->start();
                const bool complete = co_await h.environment->drain_until(std::chrono::steady_clock::now() + 5s);
                const int status = co_await h.wait_exit();
                ok = complete && status == 0 && h.environment->snapshot().terminal_output->bytes == "probe-ok:dumb";
            });
            PtyHarness broken;
            auto attachment = broken.environment->prepare_attachment();
            process::LaunchSpec spec;
            spec.executable = "/";
            bool failed_cleanly = false;
            try { subprocess::Child child(spec, broken.strand, attachment); }
            catch (const subprocess::ProcessException& error) {
                failed_cleanly = error.error_code().value() == EACCES;
            }
            broken.run([](Harness&) -> asio::awaitable<void> { co_return; });
            ::_exit(ok && failed_cleanly ? 0 : 1);
        } catch (...) { ::_exit(2); }
    }
    int status = 0;
    BOOST_REQUIRE(::waitpid(pid, &status, 0) == pid);
    BOOST_REQUIRE(WIFEXITED(status));
    BOOST_TEST(WEXITSTATUS(status) == 0);
}

BOOST_AUTO_TEST_CASE(unrelated_non_cloexec_descriptors_are_not_inherited)
{
    const int fd = ::open("/dev/null", O_RDONLY);
    BOOST_REQUIRE(fd >= 0);
    boost::scope::scope_exit cleanup([fd] { ::close(fd); });
    BOOST_TEST((::fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);
    PtyHarness h;
    h.launch_pty("fd:" + std::to_string(fd));
    h.run([](Harness& h) -> asio::awaitable<void> {
        co_await h.environment->start();
        co_await drained(h);
        BOOST_TEST(h.environment->snapshot().terminal_output->bytes == "fd-closed");
    });
    BOOST_TEST((::fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);
}
BOOST_AUTO_TEST_CASE(descendant_held_slave_has_bounded_drain_and_join)
{
    int original = 0;
    BOOST_REQUIRE(::prctl(PR_GET_CHILD_SUBREAPER, &original) == 0);
    BOOST_REQUIRE(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    pid_t descendant = -1;
    boost::scope::scope_exit cleanup([&] {
        if (descendant > 0) {
            ::kill(descendant, SIGKILL);
            while (::waitpid(descendant, nullptr, 0) < 0 && errno == EINTR) {}
        }
        ::prctl(PR_SET_CHILD_SUBREAPER, original);
    });
    PtyHarness h;
    h.launch_pty("hold-slave");
    h.run([&](Harness& h) -> asio::awaitable<void> {
        co_await h.environment->start();
        BOOST_TEST(co_await h.wait_exit() == 0);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (h.environment->snapshot().terminal_output->bytes.find('\n') == std::string_view::npos
               && std::chrono::steady_clock::now() < deadline) co_await h.pause();
        const auto bytes = h.environment->snapshot().terminal_output->bytes;
        BOOST_REQUIRE(bytes.starts_with("descendant:"));
        descendant = std::stoi(std::string(bytes.substr(11)));
        BOOST_REQUIRE(descendant > 0);
        BOOST_TEST(!(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 20ms)));
        co_await h.environment->cancel_and_join();
        BOOST_TEST(h.environment->snapshot().joined);
        BOOST_CHECK(h.environment->snapshot().terminal_output->end == StreamEnd::Stopped);
        bool resize_failed = false;
        try { co_await h.environment->resize(80, 24); }
        catch (const boost::system::system_error&) { resize_failed = true; }
        BOOST_TEST(resize_failed);
    });
}
BOOST_AUTO_TEST_CASE(terminal_operations_marshal_from_another_context)
{
    asio::io_context caller;
    auto work = asio::make_work_guard(caller);
    std::jthread caller_thread([&] { caller.run(); });
    PtyHarness h;
    h.launch_pty("echo");
    try {
        h.run([&](Harness& h) -> asio::awaitable<void> {
            co_await asio::co_spawn(caller,
                [&h]() -> asio::awaitable<void> {
                    co_await h.environment->start();
                    BOOST_CHECK_THROW((void)h.environment->snapshot(), std::logic_error);
                    co_await h.environment->enqueue_input("cross-context");
                    co_await h.environment->request_input_end();
                    BOOST_TEST(co_await h.environment->drain_until(std::chrono::steady_clock::now() + 5s));
                    co_await h.environment->cancel_and_join();
                }, asio::use_awaitable);
            BOOST_TEST(h.environment->snapshot().terminal_output->bytes ==
                "readycross-contextout-tail\r\nerr-tail\r\n");
            BOOST_TEST(h.environment->snapshot().joined);
            BOOST_TEST(co_await h.wait_exit() == 0);
        }, 3);
    } catch (...) {
        caller.stop();
        throw;
    }
    work.reset();
}

BOOST_AUTO_TEST_CASE(drain_is_interruptible_but_join_shields_caller_cancellation)
{
    PtyHarness h;
    h.launch_pty("no-read");
    h.run([](Harness& h) -> asio::awaitable<void> {
        co_await h.environment->start();
        co_await ready(h);
        asio::cancellation_signal cancel_drain;
        bool drain_done = false;
        std::exception_ptr drain_error;
        asio::co_spawn(h.strand,
            h.environment->drain_until(std::chrono::steady_clock::now() + 30s),
            asio::bind_cancellation_slot(cancel_drain.slot(),
                [&](std::exception_ptr error, bool) {
                    drain_error = error;
                    drain_done = true;
                }));
        co_await h.pause();
        cancel_drain.emit(asio::cancellation_type::all);
        while (!drain_done) co_await h.pause();
        BOOST_REQUIRE(drain_error);
        try {
            std::rethrow_exception(drain_error);
        } catch (const boost::system::system_error& error) {
            BOOST_TEST(error.code() == asio::error::operation_aborted);
        }
        BOOST_TEST(!h.environment->snapshot().output_complete);
        BOOST_TEST(h.environment->snapshot().accepting_input);

        co_await h.environment->enqueue_input(std::string(1024 * 1024, 'x'));
        co_await h.pause();
        asio::cancellation_signal cancel_join;
        bool joined = false;
        std::exception_ptr join_error;
        asio::co_spawn(h.strand, h.environment->cancel_and_join(),
            asio::bind_cancellation_slot(cancel_join.slot(),
                [&](std::exception_ptr error) {
                    join_error = error;
                    joined = true;
                }));
        asio::post(h.strand, [&] { cancel_join.emit(asio::cancellation_type::all); });
        while (!joined) co_await h.pause();
        BOOST_TEST(!join_error);
        BOOST_TEST(h.environment->snapshot().joined);
        BOOST_TEST(h.environment->snapshot().output_complete);
    });
}

BOOST_AUTO_TEST_CASE(descriptor_fallback_handles_lowered_limits_without_leaks)
{
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        // Simulate a pre-close_range kernel in an isolated process. The filter
        // only changes this syscall; it is inherited by the PTY child.
        sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_close_range, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        sock_fprog program{static_cast<unsigned short>(std::size(filter)), filter};
        if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0
            || ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) ::_exit(3);
        const int original = ::open("/dev/null", O_RDONLY);
        const int high = ::fcntl(original, F_DUPFD, 128);
        if (high < 0) ::_exit(4);
        // This high descriptor remains live after lowering both limits. A
        // limit-based scan would silently leak it into the executed child.
        rlimit limit{64, 64};
        if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) ::_exit(5);
        try {
            PtyHarness h;
            h.launch_pty("fd:" + std::to_string(high));
            bool ok = false;
            h.run([&](Harness& h) -> asio::awaitable<void> {
                co_await h.environment->start();
                const bool complete = co_await h.environment->drain_until(std::chrono::steady_clock::now() + 5s);
                const int status = co_await h.wait_exit();
                ok = complete && status == 0 && h.environment->snapshot().terminal_output->bytes == "fd-closed";
            });
            ::_exit(ok ? 0 : 1);
        } catch (...) { ::_exit(2); }
    }
    int status = 0;
    BOOST_REQUIRE(::waitpid(pid, &status, 0) == pid);
    BOOST_REQUIRE(WIFEXITED(status));
    BOOST_TEST(WEXITSTATUS(status) == 0);
}
