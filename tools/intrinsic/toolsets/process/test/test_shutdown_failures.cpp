#define BOOST_TEST_MODULE ProcessShutdownFailures
#include <boost/test/unit_test.hpp>
#include "tools/intrinsic/process/session_store.hpp"
#include "process_io/anonymous_pipe.hpp"

#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <dlfcn.h>
#include <sys/wait.h>

namespace {
std::atomic<pid_t> fault_pid{0};
std::atomic<int> wait_failures{0};
std::atomic<int> signal_failures{0};

bool consume(std::atomic<int>& count) {
    int value = count.load();
    while (value > 0) {
        if (count.compare_exchange_weak(value, value - 1)) return true;
    }
    return false;
}
}

// Interpose only the selected child's operations. No production injection API
// or test-only shared library is needed; all other processes use libc normally.
extern "C" pid_t waitpid(pid_t pid, int* status, int options) {
    using Function = pid_t (*)(pid_t, int*, int);
    static auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "waitpid"));
    if (pid == fault_pid.load() && consume(wait_failures)) {
        errno = EIO;
        return -1;
    }
    return real(pid, status, options);
}

extern "C" int kill(pid_t pid, int signal) noexcept {
    using Function = int (*)(pid_t, int);
    static auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "kill"));
    if (pid == fault_pid.load() && signal == SIGKILL && consume(signal_failures)) {
        errno = EACCES;
        return -1;
    }
    return real(pid, signal);
}

namespace asio = boost::asio;
namespace {
process::LaunchSpec spec() {
    process::LaunchSpec value;
    value.executable = "sleep";
    value.arguments = {"30"};
    value.initial_wait_timeout_milliseconds = 1;
    value.detach_on_timeout = true;
    return value;
}

void check_error(const std::exception_ptr& failure, int expected) {
    BOOST_REQUIRE(failure);
    try {
        std::rethrow_exception(failure);
    } catch (const process::ProcessException& error) {
        BOOST_TEST(error.error_code().value() == expected);
    }
}

void check_reaped(pid_t pid) {
    int status = 0;
    errno = 0;
    BOOST_TEST(::waitpid(pid, &status, WNOHANG) == -1);
    BOOST_TEST(errno == ECHILD);
}
}

BOOST_AUTO_TEST_CASE(watcher_failure_and_signal_failure_still_join_and_reap) {
    asio::io_context io;
    auto handle = std::make_shared<process::ProcessHandle>(spec(), io.get_executor());
    fault_pid = handle->pid();
    auto run = asio::co_spawn(io, [handle]() -> asio::awaitable<void> {
        // Fail the watcher's initial waitpid, then both its defensive signal
        // and shutdown's first signal. Recovery must preserve the first EIO.
        wait_failures = 1;
        signal_failures = 2;
        co_await handle->start_background_io_tasks();
        co_await handle->await_initial_execution();
        co_await handle->shutdown();
    }, asio::use_future);
    io.run(); // No stop(): every owned pipe/watcher operation must settle.
    std::exception_ptr failure;
    try { run.get(); } catch (...) { failure = std::current_exception(); }
    check_error(failure, EIO);
    BOOST_TEST(wait_failures.load() == 0);
    BOOST_TEST(signal_failures.load() == 0);
    BOOST_TEST(handle->exited());
    BOOST_TEST(handle->output_drained());
    check_reaped(handle->pid());
    fault_pid = 0;
}

BOOST_AUTO_TEST_CASE(persistent_signal_failure_joins_work_before_reporting_error) {
    asio::io_context io;
    auto handle = std::make_shared<process::ProcessHandle>(spec(), io.get_executor());
    fault_pid = handle->pid();
    std::exception_ptr first_failure;
    bool unobserved = false;
    bool drained = false;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await handle->start_background_io_tasks();
        co_await handle->await_initial_execution();
        signal_failures = 2; // Refuse both shutdown attempts.
        try { co_await handle->shutdown(); }
        catch (...) { first_failure = std::current_exception(); }
        unobserved = !handle->exited();
        drained = handle->output_drained();
        // Permission is restored. A repeat call can finish reaping, but still
        // reports the original error rather than silently changing the outcome.
        signal_failures = 0;
        co_await handle->shutdown();
    }, asio::use_future);
    io.run();
    std::exception_ptr repeated;
    try { run.get(); } catch (...) { repeated = std::current_exception(); }
    check_error(first_failure, EACCES);
    check_error(repeated, EACCES);
    BOOST_TEST(unobserved);
    BOOST_TEST(drained);
    BOOST_TEST(handle->exited());
    check_reaped(handle->pid());
    fault_pid = 0;
}

BOOST_AUTO_TEST_CASE(store_cleans_later_sessions_after_first_handle_fails) {
    asio::io_context io;
    auto store = std::make_shared<tools::intrinsic::ProcessSessionStore>(io.get_executor());
    pid_t first_pid = 0;
    pid_t second_pid = 0;
    std::size_t retained = 999;
    std::exception_ptr failure;
    auto run = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        const auto first = co_await store->spawn(spec());
        const auto second = co_await store->spawn(spec());
        first_pid = (co_await store->snapshot(first.id))->result.spec.pid;
        second_pid = (co_await store->snapshot(second.id))->result.spec.pid;
        fault_pid = first_pid;
        signal_failures = 1;
        try { co_await store->shutdown(); }
        catch (...) { failure = std::current_exception(); }
        retained = co_await store->size();
    }, asio::use_future);
    io.run();
    run.get();
    check_error(failure, EACCES);
    BOOST_TEST(signal_failures.load() == 0);
    BOOST_TEST(retained == 0u);
    check_reaped(first_pid);
    check_reaped(second_pid);
    fault_pid = 0;
}

BOOST_AUTO_TEST_CASE(emergency_cleanup_is_noexcept_and_retryable_after_native_failures)
{
    // No executor has run, so the synchronous fallback cannot race a watcher.
    asio::io_context io;
    auto handle = std::make_shared<process::ProcessHandle>(spec(), io.get_executor());
    fault_pid = handle->pid();
    wait_failures = 1;
    BOOST_CHECK_NO_THROW(handle->emergency_cleanup());
    BOOST_TEST(wait_failures.load() == 0);
    BOOST_TEST(::kill(handle->pid(), 0) == 0);

    signal_failures = 1;
    BOOST_CHECK_NO_THROW(handle->emergency_cleanup());
    BOOST_TEST(signal_failures.load() == 0);
    BOOST_TEST(::kill(handle->pid(), 0) == 0);

    BOOST_CHECK_NO_THROW(handle->emergency_cleanup());
    check_reaped(handle->pid());
    BOOST_CHECK_NO_THROW(handle->emergency_cleanup());
    fault_pid = 0;
}

// ELF interposition exercises actual rollback boundaries without introducing
// production fault switches. Successful publication delegates to the real DSO.
namespace {
std::atomic<bool> fail_publication{false};
process::ProcessId unpublished_pid = 0;
}

namespace tools::intrinsic {
void ProcessSessionStore::publish_session(SessionPtr session)
{
    if (fail_publication.exchange(false)) {
        unpublished_pid = session->handle->pid();
        throw std::bad_alloc();
    }
    using Function = void (*)(ProcessSessionStore*, SessionPtr);
    static auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT,
        "_ZN5tools9intrinsic19ProcessSessionStore15publish_sessionESt10shared_ptrINS1_7SessionEE"));
    if (!real) throw std::runtime_error("publication interposition did not resolve real method");
    real(this, std::move(session));
}
}

BOOST_AUTO_TEST_CASE(publication_failure_rolls_back_live_pumps_child_and_capacity)
{
    asio::io_context io;
    auto store = std::make_shared<tools::intrinsic::ProcessSessionStore>(io.get_executor(), 1);
    auto task = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        fail_publication = true;
        bool failed = false;
        try {
            co_await store->spawn(spec());
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        BOOST_TEST(failed);
        BOOST_TEST(!fail_publication.load());
        BOOST_TEST(unpublished_pid != 0u);
        check_reaped(unpublished_pid);
        BOOST_TEST((co_await store->size()) == 0u);
        const auto next = co_await store->spawn(spec());
        BOOST_TEST(!next.id.empty()); // Reservation was returned, capacity is one.
        co_await store->shutdown();
    }, asio::use_future);
    io.run();
    task.get();
}

namespace {
std::atomic<bool> fail_after_pumps_start{false};
}

namespace process_io {
asio::awaitable<void> AnonymousPipeIo::start()
{
    using Function = asio::awaitable<void> (*)(AnonymousPipeIo*);
    static auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT,
        "_ZN10process_io15AnonymousPipeIo5startEv"));
    if (!real) throw std::runtime_error("I/O interposition did not resolve real method");
    co_await real(this);
    if (fail_after_pumps_start.exchange(false)) {
        throw std::runtime_error("injected failure after starting I/O pumps");
    }
}
}

BOOST_AUTO_TEST_CASE(startup_failure_after_pumps_start_joins_and_reaps_before_throwing)
{
    asio::io_context io;
    auto handle = std::make_shared<process::ProcessHandle>(spec(), io.get_executor());
    auto task = asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        fail_after_pumps_start = true;
        bool failed = false;
        try {
            co_await handle->start_background_io_tasks();
        } catch (const std::runtime_error& error) {
            failed = true;
            BOOST_TEST(std::string(error.what()) == "injected failure after starting I/O pumps");
        }
        BOOST_TEST(failed);
        BOOST_TEST(!fail_after_pumps_start.load());
        BOOST_TEST(handle->exited());
        BOOST_TEST(handle->output_drained());
        check_reaped(handle->pid());
    }, asio::use_future);
    io.run(); // No context.stop(): all actually started pumps must be joined.
    task.get();
}
