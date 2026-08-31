#define BOOST_TEST_MODULE ProcessDestructiveTests
#include <boost/test/unit_test.hpp>

#include "scenario.hpp"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include <boost/asio.hpp>

// The heavier lifecycle/abuse tests for ProcessHandle: leak audits over
// many full lifecycles, a destruction storm over live children, and stdin
// writes that arrive after the input side is gone. These assume a
// disposable environment — they churn hundreds of processes, rely on
// /proc introspection, and intentionally kill children mid-flight — so
// they only engage where that is true: inside the build container
// (/.dockerenv exists) or wherever SIMPLEX_DESTRUCTIVE_TESTS=1 is set
// explicitly (manual runs, CI). On a dev host every case degrades to an
// immediate pass, keeping quick ctest iterations fast.

namespace {

const bool g_enabled = [] {
    if (std::getenv("SIMPLEX_DESTRUCTIVE_TESTS") != nullptr) return true;
    std::error_code ec;
    return std::filesystem::exists("/.dockerenv", ec);
}();

// /proc/<pid>/stat is "pid (comm) state ppid ..."; comm may contain spaces
// and parentheses, so parse from the LAST ')'.
struct StatFields {
    char state = '\0';
    long ppid = 0;
};

StatFields read_stat(long pid)
{
    StatFields out;
    std::ifstream file{std::filesystem::path{"/proc"} / std::to_string(pid) /
                       "stat"};
    std::string line;
    if (!std::getline(file, line)) return out;
    const auto tail = line.rfind(')');
    if (tail == std::string::npos) return out;
    std::istringstream rest{line.substr(tail + 2)};
    std::string state;
    long ppid = 0;
    rest >> state >> ppid;
    if (!state.empty()) out.state = state.front();
    out.ppid = ppid;
    return out;
}

// Every process whose parent is us — alive ('' filter) or zombies only.
std::vector<long> our_children(char state_filter = '\0')
{
    std::vector<long> found;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator{"/proc", ec}) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::isdigit(static_cast<unsigned char>(name.front())))
            continue;
        const long pid = std::stol(name);
        const auto fields = read_stat(pid);
        if (fields.ppid != ::getpid()) continue;
        if (state_filter != '\0' && fields.state != state_filter) continue;
        found.push_back(pid);
    }
    return found;
}

// The directory iterator's own directory fd is visible as an entry while
// counting; it appears in every measurement, so counts stay comparable.
std::size_t open_fd_count()
{
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator{"/proc/self/fd", ec})
        ++count;
    return count;
}

} // namespace

BOOST_AUTO_TEST_CASE(full_lifecycle_repeated_leaks_nothing)
{
    if (!g_enabled) {
        BOOST_TEST_MESSAGE("destructive tests disabled on this host");
        return;
    }
    // 300 complete lifecycles (spawn -> io tasks -> exit observed ->
    // destruction), all driven from ONE master coroutine under a single
    // io_context::run(). The one-run shape is deliberate: re-running a
    // returned io_context loses strand-posted wakeups (observed with asio
    // 1.3x + make_strand — round 1 of a run-per-round loop never starts),
    // so each round is a co_await, not a fresh run(). Warmup rounds absorb
    // one-time lazy opens before the fd baseline; settle pauses let each
    // batch's stragglers finish draining so the two measurements are
    // comparable. Afterwards the count must be exactly back to the
    // baseline, and no child — living or zombied — may remain.
    constexpr int warmups = 3;
    constexpr int rounds = 300;

    boost::asio::io_context io;
    auto strand = boost::asio::make_strand(io);

    std::size_t fds_before = 0;
    std::size_t fds_after = 0;
    auto settle = [&]() -> boost::asio::awaitable<void> {
        boost::asio::steady_timer pause{strand};
        pause.expires_after(std::chrono::milliseconds{200});
        co_await pause.async_wait(boost::asio::use_awaitable);
    };
    auto run_round = [&]() -> boost::asio::awaitable<void> {
        auto handle = std::make_shared<process::ProcessHandle>(
            process::LaunchSpec{
                .executable = "echo",
                .arguments = {"leak", "probe"},
                .description = "one short life",
                .timeout_milliseconds = std::uint64_t{0},
            },
            strand);
        co_await handle->start_background_io_tasks();
        const bool finished = co_await handle->await_initial_execution();
        BOOST_TEST(finished);
        BOOST_TEST(handle->exited());
        // Per-round output content is test_process_handle's business; here
        // only the resource shape matters.
    };

    auto done = boost::asio::co_spawn(
        strand,
        [&]() -> boost::asio::awaitable<void> {
            for (int i = 0; i < warmups; ++i) co_await run_round();
            co_await settle();
            fds_before = open_fd_count();

            for (int i = 0; i < rounds; ++i) co_await run_round();
            co_await settle();
            fds_after = open_fd_count();
        },
        boost::asio::use_future);

    io.run();
    done.get();

    BOOST_TEST(fds_after == fds_before);
    BOOST_TEST(our_children().empty());
}

BOOST_AUTO_TEST_CASE(destruction_storm_kills_and_reaps_every_child)
{
    if (!g_enabled) {
        BOOST_TEST_MESSAGE("destructive tests disabled on this host");
        return;
    }
    // The global-container-shutdown shape: a batch of live children whose
    // handles are destroyed together, no io task ever started. Every
    // destructor must SIGKILL and reap its own child — afterwards each pid
    // is gone entirely (kill(2) says ESRCH) and no zombie lingers.
    constexpr std::size_t storm_size = 40;

    boost::asio::io_context io;
    auto strand = boost::asio::make_strand(io);

    std::vector<std::shared_ptr<process::ProcessHandle>> handles;
    std::vector<long> pids;
    handles.reserve(storm_size);
    pids.reserve(storm_size);
    for (std::size_t i = 0; i < storm_size; ++i) {
        handles.push_back(std::make_shared<process::ProcessHandle>(
            process::LaunchSpec{
                .executable = "sleep",
                .arguments = {"30"},
                .description = "storm member",
                .timeout_milliseconds = std::uint64_t{0},
            },
            strand));
        pids.push_back(handles.back()->pid());
    }

    for (const long pid : pids)
        BOOST_TEST(::kill(pid, 0) == 0);

    handles.clear(); // the storm

    for (const long pid : pids) {
        const int probe = ::kill(pid, 0);
        BOOST_TEST(probe == -1);
        BOOST_TEST(errno == ESRCH);
    }
    BOOST_TEST(our_children().empty());
}

BOOST_AUTO_TEST_CASE(stdin_writes_after_close_or_death_are_dropped)
{
    if (!g_enabled) {
        BOOST_TEST_MESSAGE("destructive tests disabled on this host");
        return;
    }
    using process_test::run_scenario;

    // Sent after close_input(): the channel is closed, so the send fails,
    // the message is logged-and-dropped, and the child sees only what was
    // queued before the close.
    {
        auto s = run_scenario(
            process::LaunchSpec{
                .executable = "cat",
                .arguments = {},
                .description = "echo stdin back",
                .timeout_milliseconds = std::uint64_t{5000},
            },
            [](process::ProcessHandle& handle) {
                handle.write_input("first\n");
                handle.close_input();
                handle.write_input("dropped\n");
            });
        BOOST_TEST(s.finished_on_time);
        BOOST_TEST(s.handle->standard_output() == "first\n");
    }

    // Sent after the child exited and the await task auto-closed the
    // channel: same quiet drop, nothing throws, output untouched. The
    // extra run() drains the failed send's completion.
    {
        auto s = run_scenario(process::LaunchSpec{
            .executable = "echo",
            .arguments = {"done"},
            .description = "exits immediately",
            .timeout_milliseconds = std::uint64_t{5000},
        });
        BOOST_TEST(s.finished_on_time);

        s.handle->write_input("too late\n");
        s.io->run();

        BOOST_TEST(s.handle->standard_output() == "done\n");
    }
}
