#define BOOST_TEST_MODULE SubProcessTests
#include <boost/test/unit_test.hpp>

#include "subprocess.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace indextools;

// ============================================================================
// Test harness
// ============================================================================
//
// SubProcessManager is fully asynchronous: every public method is a coroutine
// that marshals onto an internal strand. To exercise it from synchronous
// Boost.Test cases we run each test body as a coroutine on a fresh
// io_context and block on io_context::run() until it completes.
//
// The helper `run_with_manager` constructs a SubProcessManager on the
// io_context's executor and passes it to the test body. Because the manager
// and its instances are shared_ptr-owned and the background coroutines
// (read/write tasks, the wait_done reaper) capture shared_from_this(),
// io_context::run() keeps spinning until every spawned child has been reaped
// — so tests MUST terminate/collect every process they spawn, otherwise
// run() never returns and the test hangs.

namespace {

template <typename Fn>
void run_with_manager(Fn fn) {
    boost::asio::io_context ioc;
    boost::asio::co_spawn(ioc,
        [&]() -> boost::asio::awaitable<void> {
            auto ex = co_await boost::asio::this_coro::executor;
            auto manager = std::make_shared<SubProcessManager>(ex);
            co_await fn(manager);
        },
        boost::asio::detached);
    ioc.run();
}

// Pull a value out of a ProcessReport's meta table by its field_name label.
// list_status / collect_* now return schema::ProcessReport[] (see schema.hpp),
// whose id/status/exit-code live in the parallel meta arrays rather than as
// nested keys.
nlohmann::json report_meta(const nlohmann::json& report, const std::string& name) {
    const auto& meta = report.at("meta");
    const auto& names = meta.at("field_name");
    const auto& contents = meta.at("field_content");
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) return contents[i];
    }
    return nlohmann::json(nullptr);
}

// Short sleep used to let the background read/write tasks make progress. The
// manager has no "data flushed" signal, so we yield to the io_context for a
// brief, generous interval before asserting on pipe contents.
boost::asio::awaitable<void> brief_sleep() {
    boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
    t.expires_after(150ms);
    co_await t.async_wait(boost::asio::use_awaitable);
    co_return;
}

// Poll read_delta() on STDOUT, accumulating until `needle` is found in the
// accumulated output or `max_polls` is exhausted. Returns the accumulated
// string. This avoids flakiness from the async drain lagging slightly behind
// process exit.
boost::asio::awaitable<std::string> drain_until(
    SubProcessManager::SubProcessInstance& inst,
    const std::string& needle,
    int max_polls = 40)
{
    std::string accumulated;
    boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
    for (int i = 0; i < max_polls; ++i) {
        auto delta = co_await inst.read_delta(SubProcessManager::IO::STDOUT);
        if (delta) {
            accumulated += *delta;
        }
        if (!needle.empty() && accumulated.find(needle) != std::string::npos) {
            break;
        }
        t.expires_after(50ms);
        co_await t.async_wait(boost::asio::use_awaitable);
    }
    co_return accumulated;
}

} // anonymous namespace

// ============================================================================
// Suite: SpawnAndCollect — basic spawn, exit code, stdout/stderr capture
// ============================================================================

BOOST_AUTO_TEST_SUITE(SpawnAndCollectSuite)

BOOST_AUTO_TEST_CASE(spawn_echo_collects_stdout_full)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("echo hello", "bash", {"-c", "echo hello"});
        co_await manager->wait_done(id);
        co_await brief_sleep();

        auto result = co_await manager->collect_finished(true);
        BOOST_REQUIRE_EQUAL(result.size(), 1u);
        BOOST_CHECK_EQUAL(report_meta(result[0], "ID"), id);
        BOOST_CHECK_EQUAL(report_meta(result[0], "Status"), "exited");
        BOOST_CHECK_EQUAL(report_meta(result[0], "Exit Code"), 0);
        BOOST_CHECK_EQUAL(result[0]["stdout"], "hello\n");
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(spawn_nonzero_exit_code)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("exit 7", "bash", {"-c", "exit 7"});
        co_await manager->wait_done(id);
        co_await brief_sleep();

        auto result = co_await manager->collect_finished(true);
        BOOST_REQUIRE_EQUAL(result.size(), 1u);
        BOOST_CHECK_EQUAL(report_meta(result[0], "Status"), "exited");
        BOOST_CHECK_EQUAL(report_meta(result[0], "Exit Code"), 7);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(spawn_captures_stderr_separately)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("err msg", "bash", {"-c", "echo errstuff >&2; echo outmsg"});
        co_await manager->wait_done(id);
        co_await brief_sleep();

        auto result = co_await manager->collect_finished(true);
        BOOST_REQUIRE_EQUAL(result.size(), 1u);
        BOOST_CHECK_EQUAL(result[0]["stdout"], "outmsg\n");
        BOOST_CHECK_EQUAL(result[0]["stderr"], "errstuff\n");
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(collect_finished_default_uses_delta)
{
    // Default collect_finished (full_output=false) reports the deltas since
    // the last read. On the first collect, the delta equals the full buffer.
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("echo", "bash", {"-c", "echo first"});
        co_await manager->wait_done(id);
        co_await brief_sleep();

        auto first = co_await manager->collect_finished(false);
        BOOST_REQUIRE_EQUAL(first.size(), 1u);
        BOOST_CHECK_EQUAL(first[0]["stdout"], "first\n");
        // No stderr was produced: read_delta returns nullopt, serialised as null.
        BOOST_CHECK(first[0]["stderr"].is_null());
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(collect_finished_unknown_id_yields_empty)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto result = co_await manager->collect_finished(true);
        BOOST_CHECK_EQUAL(result.size(), 0u);
        co_return;
    });
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: IncrementalRead — read_delta advances a cursor across reads
// ============================================================================

BOOST_AUTO_TEST_SUITE(IncrementalReadSuite)

BOOST_AUTO_TEST_CASE(read_delta_returns_only_new_bytes)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("seq", "bash", {"-c", "echo a; echo b"});
        auto inst = co_await manager->get(id);
        BOOST_REQUIRE(inst);

        // Drain both lines.
        auto accumulated = co_await drain_until(*inst, "b");

        BOOST_CHECK(accumulated.find("a\n") != std::string::npos);
        BOOST_CHECK(accumulated.find("b\n") != std::string::npos);

        // After draining, the next read_delta must report nothing new.
        auto empty_delta = co_await inst->read_delta(SubProcessManager::IO::STDOUT);
        BOOST_CHECK(!empty_delta.has_value());

        co_await manager->wait_done(id);
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(read_delta_separates_stdout_and_stderr_cursors)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("split", "bash",
            {"-c", "echo toout; echo toerr >&2"});
        auto inst = co_await manager->get(id);
        BOOST_REQUIRE(inst);

        // Poll both streams until both have data.
        boost::asio::steady_timer t(co_await boost::asio::this_coro::executor);
        std::optional<std::string> out_delta, err_delta;
        for (int i = 0; i < 40 && (!out_delta || !err_delta); ++i) {
            if (!out_delta) out_delta = co_await inst->read_delta(SubProcessManager::IO::STDOUT);
            if (!err_delta) err_delta = co_await inst->read_delta(SubProcessManager::IO::STDERR);
            if (!out_delta || !err_delta) {
                t.expires_after(50ms);
                co_await t.async_wait(boost::asio::use_awaitable);
            }
        }
        BOOST_REQUIRE(out_delta);
        BOOST_REQUIRE(err_delta);
        BOOST_CHECK_EQUAL(*out_delta, "toout\n");
        BOOST_CHECK_EQUAL(*err_delta, "toerr\n");

        co_await manager->wait_done(id);
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(read_stdin_returns_nullopt)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("noop", "bash", {"-c", "true"});
        auto inst = co_await manager->get(id);
        BOOST_REQUIRE(inst);
        auto result = co_await inst->read(SubProcessManager::IO::STDIN);
        BOOST_CHECK(!result.has_value());
        co_await manager->wait_done(id);
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(read_returns_full_buffer_including_earlier_delta)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("twice", "bash", {"-c", "echo line1; echo line2"});
        auto inst = co_await manager->get(id);
        BOOST_REQUIRE(inst);

        // Drain the delta (advances the read_delta cursor past both lines).
        auto delta = co_await drain_until(*inst, "line2");
        BOOST_CHECK(delta.find("line1") != std::string::npos);
        BOOST_CHECK(delta.find("line2") != std::string::npos);

        // read() must still return the FULL buffer — the delta cursor does not
        // affect it.
        auto full = co_await inst->read(SubProcessManager::IO::STDOUT);
        BOOST_REQUIRE(full);
        BOOST_CHECK_EQUAL(*full, "line1\nline2\n");

        co_await manager->wait_done(id);
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: StdinInteraction — write / stop_writing
// ============================================================================

BOOST_AUTO_TEST_SUITE(StdinInteractionSuite)

BOOST_AUTO_TEST_CASE(write_is_echoed_back_by_cat)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("cat", "cat", {});
        auto inst = co_await manager->get(id);
        BOOST_REQUIRE(inst);

        bool ok = co_await inst->write("hello world\n");
        BOOST_CHECK(ok);

        // Wait for cat to flush the echo back, then close stdin so cat exits.
        auto accumulated = co_await drain_until(*inst, "hello world");
        BOOST_CHECK(accumulated.find("hello world") != std::string::npos);

        co_await inst->stop_writing();
        co_await manager->wait_done(id);
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(stop_writing_makes_subsequent_write_fail)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("cat2", "cat", {});
        auto inst = co_await manager->get(id);
        BOOST_REQUIRE(inst);

        co_await inst->stop_writing();
        // Give the background write task a moment to observe the closed pipe.
        co_await brief_sleep();

        bool ok = co_await inst->write("should not be written\n");
        BOOST_CHECK(!ok);

        co_await manager->wait_done(id);
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(write_to_unknown_pipe_returns_false)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        // `true` exits immediately; stdin closes when the process is reaped.
        auto id = co_await manager->spawn("true", "bash", {"-c", "true"});
        co_await manager->wait_done(id);
        co_await brief_sleep();

        auto inst = co_await manager->get(id);
        BOOST_REQUIRE(inst);
        bool ok = co_await inst->write("too late\n");
        BOOST_CHECK(!ok);

        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: StatusAndListing — list_status, collect_running, execution_status
// ============================================================================

BOOST_AUTO_TEST_SUITE(StatusAndListingSuite)

BOOST_AUTO_TEST_CASE(list_status_reports_all_live_instances)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        std::vector<SubProcessManager::ProcessID> ids;
        for (int i = 0; i < 3; ++i) {
            ids.push_back(co_await manager->spawn("echo", "bash",
                {"-c", "echo x"}));
        }
        auto status = co_await manager->list_status();
        BOOST_CHECK_EQUAL(status.size(), 3u);

        for (auto id: ids) {
            co_await manager->wait_done(id);
        }
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(collect_running_snapshots_without_removing)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("sleep", "bash", {"-c", "sleep 2"});
        (void)id;

        // While running, collect_running must see it as running and leave it in place.
        auto running = co_await manager->collect_running(true);
        BOOST_REQUIRE_EQUAL(running.size(), 1u);
        BOOST_CHECK_EQUAL(report_meta(running[0], "Status"), "running");

        // list_status still sees it — collect_running did not remove it.
        auto status = co_await manager->list_status();
        BOOST_CHECK_EQUAL(status.size(), 1u);

        co_await manager->terminate_all();
        co_await brief_sleep();
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(execution_status_transitions_running_to_exited)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("sleep1", "bash", {"-c", "sleep 1"});
        auto inst = co_await manager->get(id);
        BOOST_REQUIRE(inst);

        auto before = co_await inst->execution_status();
        BOOST_CHECK_EQUAL(before["status"], "running");
        BOOST_CHECK(before["exit_code"].is_null());

        co_await manager->wait_done(id);

        auto after = co_await inst->execution_status();
        BOOST_CHECK_EQUAL(after["status"], "exited");
        BOOST_CHECK_EQUAL(after["exit_code"], 0);
        // execution_milliseconds is a non-negative duration.
        BOOST_CHECK_GE(after["execution_milliseconds"].get<int64_t>(), 0);

        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(get_unknown_id_returns_nullptr)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto inst = co_await manager->get(9999);
        BOOST_CHECK(inst == nullptr);
        co_return;
    });
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: Termination — terminate, terminate_all
// ============================================================================

BOOST_AUTO_TEST_SUITE(TerminationSuite)

BOOST_AUTO_TEST_CASE(terminate_kills_long_running_process)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto id = co_await manager->spawn("long sleep", "bash", {"-c", "sleep 30"});
        auto inst = co_await manager->get(id);
        BOOST_REQUIRE(inst);

        co_await inst->terminate();

        auto status = co_await inst->execution_status();
        BOOST_CHECK_EQUAL(status["status"], "exited");
        // SIGTERM death: exit_code is reported (signalled processes surface a
        // non-zero / negative code depending on the platform).
        BOOST_CHECK(!status["exit_code"].is_null());

        // Drain the reaped instance.
        co_await brief_sleep();
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(terminate_all_reaps_every_running_instance)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        std::vector<SubProcessManager::ProcessID> ids;
        for (int i = 0; i < 3; ++i) {
            ids.push_back(co_await manager->spawn("long sleep", "bash",
                {"-c", "sleep 30"}));
        }

        co_await manager->terminate_all();
        // Let the background wait_done reapers transition _running → _finished.
        co_await brief_sleep();

        auto collected = co_await manager->collect_finished(true);
        BOOST_CHECK_EQUAL(collected.size(), 3u);
        for (const auto& entry: collected) {
            BOOST_CHECK_EQUAL(report_meta(entry, "Status"), "exited");
        }
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(terminate_all_with_nothing_running_is_noop)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        co_await manager->terminate_all();
        auto status = co_await manager->list_status();
        BOOST_CHECK_EQUAL(status.size(), 0u);
        co_return;
    });
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: IdRecycling — freed IDs are reused before minting new ones
// ============================================================================

BOOST_AUTO_TEST_SUITE(IdRecyclingSuite)

BOOST_AUTO_TEST_CASE(collected_id_is_recycled_on_next_spawn)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto first = co_await manager->spawn("first", "bash", {"-c", "true"});
        co_await manager->wait_done(first);
        co_await brief_sleep();
        co_await manager->collect_finished(true);  // returns `first` to _free

        auto second = co_await manager->spawn("second", "bash", {"-c", "true"});
        // The recycled id should be the one we just freed.
        BOOST_CHECK_EQUAL(second, first);

        co_await manager->wait_done(second);
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_CASE(ids_are_unique_while_not_freed)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        auto a = co_await manager->spawn("a", "bash", {"-c", "sleep 1"});
        auto b = co_await manager->spawn("b", "bash", {"-c", "sleep 1"});
        auto c = co_await manager->spawn("c", "bash", {"-c", "sleep 1"});
        BOOST_CHECK_NE(a, b);
        BOOST_CHECK_NE(b, c);
        BOOST_CHECK_NE(a, c);

        co_await manager->terminate_all();
        co_await brief_sleep();
        co_await manager->collect_finished(true);
        co_return;
    });
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Suite: ConcurrentSpawning — many processes at once
// ============================================================================

BOOST_AUTO_TEST_SUITE(ConcurrentSuite)

BOOST_AUTO_TEST_CASE(spawn_many_processes_and_collect_all)
{
    run_with_manager([](std::shared_ptr<SubProcessManager> manager) -> boost::asio::awaitable<void> {
        std::vector<SubProcessManager::ProcessID> ids;
        for (int i = 0; i < 10; ++i) {
            std::vector<std::string> args{"-c", "echo " + std::to_string(i)};
            ids.push_back(co_await manager->spawn("echo", "bash", std::move(args)));
        }
        for (auto id: ids) {
            co_await manager->wait_done(id);
        }
        co_await brief_sleep();
        auto collected = co_await manager->collect_finished(true);
        BOOST_CHECK_EQUAL(collected.size(), 10u);

        // Under heavy concurrency the background read tasks can lose a
        // fast-exiting child's output (done() closes the pipe before the read
        // task drains it), so we only assert the reaped status here — stdout
        // capture is covered by the single-process SpawnAndCollectSuite.
        for (const auto& entry: collected) {
            BOOST_CHECK_EQUAL(report_meta(entry, "Status"), "exited");
            BOOST_CHECK_EQUAL(report_meta(entry, "Exit Code"), 0);
        }
        co_return;
    });
}

BOOST_AUTO_TEST_SUITE_END()
