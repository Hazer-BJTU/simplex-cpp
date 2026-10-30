#define BOOST_TEST_MODULE FileWork
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/file_work.hpp"
#include "tools/intrinsic/reading/tools.hpp"
#include "tools/intrinsic/editing/tools.hpp"
#include "tools/invoke_exception.hpp"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
namespace asio = boost::asio;
using namespace std::chrono_literals;

// Pause the first source read, not the executor or unrelated file traffic.
// A timeout bounds failures even if the regression blocks the control strand.
std::atomic<bool> armed{false};
std::atomic<bool> entered{false};
ino_t source_inode = 0;
dev_t source_device = 0;
std::mutex gate_mutex;
std::condition_variable gate_changed;
bool released = false;
std::function<void()> on_read;

void release_read()
{
    {
        std::lock_guard lock(gate_mutex);
        released = true;
    }
    gate_changed.notify_all();
}

struct Scratch {
    std::filesystem::path root;
    Scratch()
    {
        auto pattern = (std::filesystem::temp_directory_path() / "simplex-file-work-XXXXXX").string();
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot create file work test directory");
        root = directory;
        std::ofstream(root / "source") << "old\n";
        // Sparse 128 MiB input reproduces a tiny selection with whole-file totals.
        std::filesystem::resize_file(root / "source", 128 * 1024 * 1024);
    }
    ~Scratch()
    {
        armed = false;
        on_read = {};
        release_read();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void pause_read()
    {
        struct stat info {};
        BOOST_REQUIRE(::stat((root / "source").c_str(), &info) == 0);
        source_inode = info.st_ino;
        source_device = info.st_dev;
        entered = false;
        released = false;
        armed = true;
    }
};

void responsive_tool(bool edit)
{
    Scratch scratch;
    asio::io_context io;
    const auto strand = asio::make_strand(io);
    std::stop_source stop;
    auto work = std::make_shared<tools::intrinsic::FileWork>([&] { return stop.get_token(); });
    tools::intrinsic::ReadTextTool reader(work);
    tools::intrinsic::StrReplaceEditTool editor(work);
    model_io::InvokeQuery query;
    query.arguments = edit
        ? nlohmann::json{{"path", (scratch.root / "source").string()},
                         {"old_text", "old"}, {"new_text", "new"}}
        : nlohmann::json{{"path", (scratch.root / "source").string()},
                         {"mode", "bytes"}, {"format", "hex_escaped"},
                         {"start", 0}, {"count", 1}};
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::duration latency{};
    on_read = [&] {
        started = std::chrono::steady_clock::now();
        asio::post(strand, [&] {
            latency = std::chrono::steady_clock::now() - started;
            stop.request_stop();
            release_read();
        });
    };
    scratch.pause_read();
    auto result = asio::co_spawn(strand, edit ? editor.invoke(query) : reader.invoke(query),
                                  asio::use_future);
    io.run(); // Must wait for the scan to exit and its result to be delivered.
    armed = false;
    BOOST_TEST(entered.load());
    BOOST_TEST(latency < 500ms);
    BOOST_CHECK_EXCEPTION(result.get(), tools::InvokeException,
        [](const auto& failure) {
            return std::string(failure.what()).find("cancelled") != std::string::npos;
        });
    BOOST_CHECK(std::chrono::steady_clock::now() - started < 2s);
    std::ifstream source(scratch.root / "source");
    std::string first;
    std::getline(source, first);
    BOOST_TEST(first == "old");
    BOOST_TEST(std::distance(std::filesystem::directory_iterator(scratch.root),
                              std::filesystem::directory_iterator{}) == 1);
}
} // namespace

extern "C" ssize_t read(int descriptor, void* output, size_t count)
{
    if (armed.load()) {
        struct stat info {};
        if (::fstat(descriptor, &info) == 0 && info.st_ino == source_inode
            && info.st_dev == source_device && !entered.exchange(true)) {
            if (on_read) on_read();
            std::unique_lock lock(gate_mutex);
            gate_changed.wait_for(lock, 3s, [] { return released; });
        }
    }
    return static_cast<ssize_t>(::syscall(SYS_read, descriptor, output, count));
}

BOOST_AUTO_TEST_CASE(read_control_remains_responsive_and_cancel_joins) { responsive_tool(false); }
BOOST_AUTO_TEST_CASE(edit_control_remains_responsive_and_cancel_joins) { responsive_tool(true); }

BOOST_AUTO_TEST_CASE(bounded_queue_and_asio_cancellation_preserve_joined_result)
{
    asio::io_context io;
    auto work = std::make_shared<tools::intrinsic::FileWork>(
        tools::intrinsic::FileWork::StopProvider{}, 1);
    std::promise<void> release;
    const auto wait = release.get_future().share();
    std::atomic<bool> done{false};
    asio::cancellation_signal cancel;
    bool still_joining = false;
    bool caller_cancelled = false;
    std::future<int> running;
    auto operation = [&]() -> asio::awaitable<int> {
        const auto value = co_await work->run([&](std::stop_token) {
            asio::post(io, [&] {
                cancel.emit(asio::cancellation_type::terminal);
                still_joining = running.wait_for(0s) != std::future_status::ready;
                release.set_value();
            });
            wait.wait_for(3s);
            done = true;
            return 42;
        });
        const auto state = co_await asio::this_coro::cancellation_state;
        caller_cancelled = state.cancelled() != asio::cancellation_type::none;
        co_return value;
    };
    running = asio::co_spawn(io, operation,
        asio::bind_cancellation_slot(cancel.slot(), asio::use_future));
    auto excess = asio::co_spawn(io, work->run([](std::stop_token) { return 0; }), asio::use_future);
    io.run();
    BOOST_TEST(still_joining);
    BOOST_TEST(caller_cancelled);
    BOOST_TEST(done.load());
    BOOST_TEST(running.get() == 42);
    BOOST_CHECK_EXCEPTION(excess.get(), std::runtime_error, [](const auto& error) {
        return std::string(error.what()).find("queue is full") != std::string::npos;
    });
    // Capacity is returned even for a failure; no background work survives reset.
    io.restart();
    auto again = asio::co_spawn(io, work->run([](std::stop_token) { return 7; }), asio::use_future);
    io.run();
    BOOST_TEST(again.get() == 7);
    work.reset();
}

BOOST_AUTO_TEST_CASE(queued_job_keeps_original_run_token_and_does_not_start_after_cancel)
{
    asio::io_context io;
    std::stop_source source;
    auto work = std::make_shared<tools::intrinsic::FileWork>([&] { return source.get_token(); }, 2);
    std::promise<void> release;
    auto wait = release.get_future().share();
    bool queued_started = false;
    auto first = asio::co_spawn(io, work->run([&, wait](std::stop_token) {
        asio::post(io, [&] {
            source.request_stop();
            source = std::stop_source(); // Must not revive already queued work.
            release.set_value();
        });
        wait.wait_for(3s);
        return 1;
    }), asio::use_future);
    auto queued = asio::co_spawn(io, work->run([&](std::stop_token) {
        queued_started = true;
        return 2;
    }), asio::use_future);
    io.run();
    BOOST_TEST(first.get() == 1); // Late stop never hides a completed result.
    BOOST_CHECK_EXCEPTION(queued.get(), std::system_error, [](const auto& error) {
        return error.code() == std::errc::operation_canceled;
    });
    BOOST_TEST(!queued_started);
}

BOOST_AUTO_TEST_CASE(lazy_job_owns_executor_until_settlement)
{
    asio::io_context io;
    auto work = std::make_shared<tools::intrinsic::FileWork>();
    std::weak_ptr<tools::intrinsic::FileWork> lifetime = work;
    auto pending = work->run([](std::stop_token) { return 9; });
    work.reset();
    BOOST_TEST(!lifetime.expired());
    auto result = asio::co_spawn(io, std::move(pending), asio::use_future);
    io.run();
    BOOST_TEST(result.get() == 9);
    BOOST_TEST(lifetime.expired());
}
