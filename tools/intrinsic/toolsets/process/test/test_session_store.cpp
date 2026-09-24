#define BOOST_TEST_MODULE ProcessSessionStoreTests
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/process/session_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>
#include <cerrno>
#include <sys/types.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

// Tests for the session table: id minting (monotonic, never reused), the
// delta/full read distinction and its cursor, the refusal to release a live
// child, waiting for ANY of a set with and without a deadline, the difference
// between "the
// child exited" and "its output is all here", and the shutdown that must not
// leak children.
//
// Children are the same harmless coreutils process/'s own tests use (echo /
// cat / sleep / true), and every case drives one store on a private
// io_context — so what a case observes is only what its own calls made.

namespace asio = boost::asio;
using tools::intrinsic::OutputRead;
using tools::intrinsic::OutputStream;
using tools::intrinsic::ProcessSessionStore;
using tools::intrinsic::SessionSnapshot;

namespace {

/// The store under test, driven by a context that runs the way a HOST's does:
/// continuously, on its own thread, until the fixture tears it down.
///
/// It cannot be driven the way process/'s own tests drive theirs — spawn work,
/// io.run() to quiescence, assert on the aftermath. The store keeps a detached
/// await task per session, which is perpetual work: io.run() would not return
/// until every child had exited, so "the context ran out of work" is not a
/// synchronisation point here. Instead each call is co_spawned with
/// use_future and the TEST THREAD blocks on that future — which is also how a
/// host calls the store, so the cases exercise the real threading rather than
/// a single-threaded special case.
///
/// Declaration order matters twice over: the guard and thread stop before the
/// store is destroyed (see the destructor), and the context outlives both.
struct Fixture {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard;
    std::thread runner;
    std::shared_ptr<ProcessSessionStore> store;

    explicit Fixture(std::size_t max_sessions =
                         ProcessSessionStore::kDefaultMaxSessions)
        : guard(asio::make_work_guard(io)),
          runner([this] { io.run(); }),
          store(std::make_shared<ProcessSessionStore>(
              io.get_executor(), max_sessions))
    {}

    ~Fixture() { shutdown(); }

    Fixture(const Fixture&) = delete;
    Fixture& operator = (const Fixture&) = delete;

    /// Quiesce workers before the destructor's fallback raw-pid cleanup.
    /// Idempotent, so a case may call it early to observe teardown.
    void shutdown()
    {
        guard.reset();
        io.stop();
        if (runner.joinable()) runner.join();
        store.reset();
    }

    /// Run one coroutine over the store on the store's context and block the
    /// test thread until it answers.
    template <typename Awaitable>
    auto run(Awaitable&& work)
    {
        return asio::co_spawn(io, std::forward<Awaitable>(work),
                              asio::use_future)
            .get();
    }

    /// Spawn and keep only the id, WITHOUT an initial wait — what most cases
    /// below mean by "there is a child": they go on to drive it themselves
    /// (read it, write to it, kill it), so waiting on the launch would settle
    /// the very thing they are about to test. The cases that care about the
    /// window call spawn() directly and read SpawnResult::finished.
    std::string spawn_id(process::LaunchSpec spec)
    {
        spec.initial_wait_timeout_milliseconds =
            ProcessSessionStore::kNoInitialWait;
        return run(store->spawn(std::move(spec))).id;
    }

    /// Wait for ONE session through wait_for_any() and hand back its snapshot:
    /// a list of one is the one-session form of the call, and the cases below
    /// that just need a child terminal want nothing else from the wait.
    /// nullopt when no session has that id, which is how the store's other
    /// one-id calls answer too.
    std::optional<SessionSnapshot> wait_for_one(const std::string& id,
                                                std::uint64_t timeout = 5000)
    {
        const auto outcome = run(store->wait_for_any({id}, timeout));
        if (outcome.snapshots.empty()) {
            return std::nullopt;
        }
        return outcome.snapshots.front();
    }
};

process::LaunchSpec spec_for(std::string executable,
                             std::vector<std::string> arguments = {},
                             std::string description = "test child")
{
    process::LaunchSpec spec;
    spec.executable = std::move(executable);
    spec.arguments = std::move(arguments);
    spec.description = std::move(description);
    return spec;
}

/// Multiple workers exercise table locking and per-session strand isolation.
struct MultiWorkerFixture {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard;
    std::vector<std::thread> runners;
    std::shared_ptr<ProcessSessionStore> store;

    explicit MultiWorkerFixture(std::size_t workers = 3)
        : guard(asio::make_work_guard(io))
    {
        runners.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            runners.emplace_back([this] { io.run(); });
        }
        store = std::make_shared<ProcessSessionStore>(io.get_executor());
    }

    ~MultiWorkerFixture() { shutdown(); }

    MultiWorkerFixture(const MultiWorkerFixture&) = delete;
    MultiWorkerFixture& operator = (const MultiWorkerFixture&) = delete;

    /// Join workers before fallback cleanup so it cannot race child reaping.
    void shutdown()
    {
        guard.reset();
        io.stop();
        for (std::thread& runner : runners) {
            if (runner.joinable()) runner.join();
        }
        store.reset();
    }

    template <typename Awaitable>
    auto run(Awaitable&& work)
    {
        return asio::co_spawn(io, std::forward<Awaitable>(work),
                              asio::use_future)
            .get();
    }

    std::string spawn_id(process::LaunchSpec spec)
    {
        spec.initial_wait_timeout_milliseconds =
            ProcessSessionStore::kNoInitialWait;
        return run(store->spawn(std::move(spec))).id;
    }

    /// The single-worker fixture's helper, over this fixture's context: the
    /// multi-worker cases need a child terminal too, and a list of one is the
    /// one-session form of wait_for_any().
    std::optional<SessionSnapshot> wait_for_one(const std::string& id,
                                                std::uint64_t timeout = 5000)
    {
        const auto outcome = run(store->wait_for_any({id}, timeout));
        if (outcome.snapshots.empty()) {
            return std::nullopt;
        }
        return outcome.snapshots.front();
    }
};

/// Whether a pid still names a RUNNABLE process — not merely a pid the
/// kernel still knows.
///
/// Note for the caller: this is a POINT-IN-TIME probe, and signal delivery is
/// asynchronous — kill() returning does not mean the target has already been
/// torn down. Use wait_until_not_running() to assert a death.
///
/// kill(pid, 0) is not enough on its own: a killed-but-unreaped child is a
/// zombie, and a zombie still answers it. That case matters here, because the
/// destructor's last-resort path signals the child while its handle (kept
/// alive by its own await task) is still the parent that has not reaped it —
/// so "dead" legitimately means "zombie" there. /proc/<pid>/stat's state
/// field tells the two apart: 'Z' is dead and awaiting a reap, anything else
/// is a process still doing something.
bool process_running(pid_t pid)
{
    std::ifstream stat(std::format("/proc/{}/stat", static_cast<int>(pid)));
    if (!stat.is_open()) {
        return false; // gone entirely
    }
    std::string line;
    std::getline(stat, line);
    // The comm field is parenthesised and may contain spaces, so the state
    // char is the first non-space after the LAST ')'.
    const auto comm_end = line.rfind(')');
    if (comm_end == std::string::npos || comm_end + 2 >= line.size()) {
        return false;
    }
    return line[comm_end + 2] != 'Z';
}

/// Wait (briefly) for `pid` to stop being a runnable process.
///
/// A kill is asynchronous: the contract the store offers is "the signal was
/// sent", and the kernel tears the target down a moment later. Asserting on a
/// single probe therefore fails intermittently — it did, roughly one run in
/// ten — so a death is asserted by waiting for it, with a ceiling far above
/// the real latency (milliseconds) so a genuinely leaked child still fails
/// the case rather than hanging it.
bool wait_until_not_running(pid_t pid,
                            std::chrono::milliseconds limit =
                                std::chrono::seconds{5})
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (process_running(pid)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

} // namespace

BOOST_AUTO_TEST_CASE(spawn_registers_a_session_and_reports_it)
{
    Fixture f;
    const auto id = f.spawn_id(spec_for("echo", {"hello"}));

    // Session IDs are opaque and unique across store incarnations.
    BOOST_TEST(!id.empty());

    const auto snapshot = f.run(f.store->snapshot(id));
    BOOST_TEST_REQUIRE(snapshot.has_value());
    BOOST_TEST(snapshot->id == id);
    BOOST_TEST(snapshot->result.spec.executable == std::string("echo"));
    BOOST_TEST(snapshot->result.spec.pid > 0);
    BOOST_TEST(f.run(f.store->size()) == std::size_t{1});
}

BOOST_AUTO_TEST_CASE(spawn_answers_at_once_when_the_child_finishes_in_its_window)
{
    // The window's whole purpose: a quick command is already TERMINAL when the
    // spawn answers, so a caller gets its result without a second call. This is
    // ProcessHandle::await_initial_execution() doing the job it was built for,
    // rather than the store disabling it.
    Fixture f;
    process::LaunchSpec spec = spec_for("echo", {"done-already"});
    spec.initial_wait_timeout_milliseconds = 5000;

    const auto spawned = f.run(f.store->spawn(std::move(spec)));
    BOOST_TEST(spawned.finished);
    // And the SECOND fact, reported separately: the capture behind that exit
    // is complete, so the caller can treat what it reads as the whole output.
    BOOST_TEST(spawned.output_drained);

    const auto snapshot = f.run(f.store->snapshot(spawned.id));
    BOOST_TEST_REQUIRE(snapshot.has_value());
    BOOST_TEST(snapshot->exited);
    BOOST_TEST(snapshot->output_drained);
    BOOST_TEST(snapshot->result.execution.exit_code.value() == 0);
    // And the output is complete, not still in flight — the spawn waits out the
    // drain for the same reason wait_for_any() does.
    BOOST_TEST(snapshot->result.stdout_text.value() == "done-already\n");
}

BOOST_AUTO_TEST_CASE(spawn_detaches_a_child_that_outlives_its_window)
{
    // The other half: a child still running when the window closes is DETACHED
    // and becomes an ordinary live session. The window is a grace period, never
    // a lifetime cap — so the sleep must still be alive afterwards.
    Fixture f;
    process::LaunchSpec spec = spec_for("sleep", {"5"});
    spec.initial_wait_timeout_milliseconds = 50;
    // Asked for the child to be killed at the deadline; the store forces detach
    // regardless, because a killed child would leave an id naming nothing.
    spec.detach_on_timeout = false;

    const auto spawned = f.run(f.store->spawn(std::move(spec)));
    BOOST_TEST(!spawned.finished);

    const auto snapshot = f.run(f.store->snapshot(spawned.id));
    BOOST_TEST_REQUIRE(snapshot.has_value());
    BOOST_TEST(!snapshot->exited);
    BOOST_TEST(snapshot->result.spec.detach_on_timeout == true);
    // Still there to be come back to, which is what makes it a session.
    BOOST_TEST(f.run(f.store->size()) == std::size_t{1});
    f.run(f.store->terminate(spawned.id, false));
}

BOOST_AUTO_TEST_CASE(a_zero_window_does_not_wait_at_all)
{
    // 0 means "wait indefinitely" to the handle, which would hang a spawn on a
    // child that never exits. The store reads it as the opposite — do not wait —
    // so "start it and give me the id" stays expressible.
    Fixture f;
    process::LaunchSpec spec = spec_for("sleep", {"30"});
    spec.initial_wait_timeout_milliseconds = ProcessSessionStore::kNoInitialWait;

    const auto spawned = f.run(f.store->spawn(std::move(spec)));
    BOOST_TEST(!spawned.finished);
    BOOST_TEST(!f.run(f.store->snapshot(spawned.id))->exited);
    f.run(f.store->terminate(spawned.id, false));
}

BOOST_AUTO_TEST_CASE(a_session_is_findable_while_its_spawn_is_still_waiting)
{
    // The ordering the spawn's shape exists for: the session is registered on
    // the table mutex BEFORE the wait suspends, so a concurrent caller sees
    // the child that is running rather than a table that has not caught up. If
    // registration happened after the wait, this poll would find nothing.
    Fixture f;
    process::LaunchSpec spec = spec_for("sleep", {"1"});
    spec.initial_wait_timeout_milliseconds = 5000;

    auto spawning = asio::co_spawn(f.io, f.store->spawn(std::move(spec)),
                                   asio::use_future);
    // The spawn is now parked on its window. The table must already know.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    const auto listed = f.run(f.store->snapshots({}));
    BOOST_TEST_REQUIRE(listed.size() == std::size_t{1});
    BOOST_TEST(!listed[0].exited);

    const auto spawned = spawning.get();
    BOOST_TEST(spawned.finished); // the 1s sleep fits the 5s window
    BOOST_TEST(spawned.id == listed[0].id);
}

BOOST_AUTO_TEST_CASE(a_failed_launch_registers_nothing)
{
    Fixture f;
    bool threw = false;
    try {
        f.spawn_id(spec_for("simplex-no-such-executable-xyz"));
    } catch (const process::ProcessException& e) {
        threw = true;
        // Passed through untranslated, so the launch context survives.
        BOOST_CHECK(e.stage() ==
                    process::ProcessException::Stage::ResolveExecutable);
    }
    BOOST_TEST(threw);
    // The table is untouched, and the id was never spent: the next spawn
    // still receives a valid opaque identity.
    BOOST_TEST(f.run(f.store->size()) == std::size_t{0});
    const auto id = f.spawn_id(spec_for("true"));
    BOOST_TEST(!id.empty());
}

BOOST_AUTO_TEST_CASE(the_session_cap_refuses_further_spawns)
{
    Fixture f{2};
    const auto first = f.spawn_id(spec_for("sleep", {"5"}));
    const auto second = f.spawn_id(spec_for("sleep", {"5"}));

    bool threw = false;
    try {
        f.spawn_id(spec_for("sleep", {"5"}));
    } catch (const std::runtime_error& e) {
        threw = true;
        // The message says what to do about it, since a model reads it.
        BOOST_TEST(std::string(e.what()).find("full") != std::string::npos);
    }
    BOOST_TEST(threw);
    BOOST_TEST(f.run(f.store->size()) == std::size_t{2});

    f.run(f.store->terminate(first, false));
    f.run(f.store->terminate(second, false));
}

BOOST_AUTO_TEST_CASE(delta_reads_advance_the_cursor_and_full_reads_do_not)
{
    Fixture f;
    const auto id = f.spawn_id(spec_for("echo", {"one"}));
    const auto waited = f.wait_for_one(id, 5000);
    BOOST_TEST_REQUIRE(waited.has_value());
    BOOST_TEST_REQUIRE(waited->exited);

    // First delta: everything captured so far.
    const auto first = f.run(f.store->read_output(id, OutputStream::Both, false));
    BOOST_TEST_REQUIRE(first.has_value());
    BOOST_TEST(first->standard_output.text == std::string("one\n"));
    BOOST_TEST(first->stdout_cursor == std::size_t{4});

    // Second delta: nothing new. This is the property a poll loop depends on.
    const auto second = f.run(f.store->read_output(id, OutputStream::Both, false));
    BOOST_TEST_REQUIRE(second.has_value());
    BOOST_TEST(second->standard_output.text.empty());
    BOOST_TEST(second->stdout_cursor == std::size_t{4});

    // A full read returns everything AND leaves the cursor alone, so it
    // cannot consume a concurrent delta reader's unread bytes.
    const auto full = f.run(f.store->read_output(id, OutputStream::Both, true));
    BOOST_TEST_REQUIRE(full.has_value());
    BOOST_TEST(full->standard_output.text == std::string("one\n"));
    BOOST_TEST(full->stdout_cursor == std::size_t{4});

    const auto after_full =
        f.run(f.store->read_output(id, OutputStream::Both, false));
    BOOST_TEST_REQUIRE(after_full.has_value());
    BOOST_TEST(after_full->standard_output.text.empty());
}

BOOST_AUTO_TEST_CASE(the_two_streams_carry_independent_cursors)
{
    Fixture f;
    // Writes to stdout only, so a stdout read must not move the stderr
    // cursor (and vice versa).
    const auto id = f.spawn_id(spec_for("echo", {"out"}));
    BOOST_TEST_REQUIRE(f.wait_for_one(id, 5000)->exited);

    // Reading stderr alone leaves stdout unread.
    const auto errs = f.run(f.store->read_output(id, OutputStream::Stderr, false));
    BOOST_TEST_REQUIRE(errs.has_value());
    BOOST_TEST(errs->standard_error.text.empty());
    BOOST_TEST(errs->stdout_cursor == std::size_t{0});

    const auto outs = f.run(f.store->read_output(id, OutputStream::Stdout, false));
    BOOST_TEST_REQUIRE(outs.has_value());
    BOOST_TEST(outs->standard_output.text == std::string("out\n"));
    BOOST_TEST(outs->stdout_cursor == std::size_t{4});
}

BOOST_AUTO_TEST_CASE(reading_an_unknown_session_answers_nothing)
{
    Fixture f;
    BOOST_TEST(!f.run(f.store->read_output("proc_404", OutputStream::Both, false))
                    .has_value());
    BOOST_TEST(!f.run(f.store->snapshot("proc_404")).has_value());
    // A wait over an id that names nothing is not an error: it is a selection
    // that resolves to nothing, and it answers at once with nothing in it
    // (rather than holding the caller for the deadline).
    BOOST_TEST(!f.wait_for_one("proc_404", 10).has_value());
    BOOST_TEST(!f.run(f.store->write_input("proc_404", "x", false)));
    BOOST_TEST(!f.run(f.store->terminate("proc_404", false)));
    BOOST_TEST(!f.run(f.store->release("proc_404")));
}

BOOST_AUTO_TEST_CASE(written_input_reaches_the_child)
{
    Fixture f;
    const auto id = f.spawn_id(spec_for("cat"));

    // Two writes then EOF: cat echoes both lines and exits on the close.
    BOOST_TEST(f.run(f.store->write_input(id, "line one\n", false)));
    BOOST_TEST(f.run(f.store->write_input(id, "line two\n", true)));

    const auto waited = f.wait_for_one(id, 5000);
    BOOST_TEST_REQUIRE(waited.has_value());
    BOOST_TEST(waited->exited);
    BOOST_TEST(waited->result.execution.exit_code.value() == 0);

    const auto read = f.run(f.store->read_output(id, OutputStream::Stdout, true));
    BOOST_TEST_REQUIRE(read.has_value());
    BOOST_TEST(read->standard_output.text ==
               std::string("line one\nline two\n"));
}

BOOST_AUTO_TEST_CASE(a_deadline_ends_the_wait_and_changes_nothing)
{
    Fixture f;
    const auto id = f.spawn_id(spec_for("sleep", {"30"}));

    const auto before = std::chrono::steady_clock::now();
    const auto waited = f.run(f.store->wait_for_any({id}, 150));
    const auto elapsed = std::chrono::steady_clock::now() - before;

    BOOST_TEST_REQUIRE(waited.snapshots.size() == std::size_t{1});
    // A timeout is a result, not a failure: the child is reported as still
    // running, and it really is. The flags say which of the two ended the wait,
    // and `waited_milliseconds` says the deadline is the one that did.
    BOOST_TEST(waited.timed_out);
    BOOST_TEST(!waited.finished);
    BOOST_TEST(!waited.snapshots.front().exited);
    BOOST_CHECK(waited.snapshots.front().result.execution.state ==
                process::ProcessState::Running);
    BOOST_CHECK(elapsed < std::chrono::seconds{5});
    BOOST_CHECK(waited.waited_milliseconds >= 150);

    // Still waitable afterwards — the deadline ended the wait, not the child.
    BOOST_TEST(f.run(f.store->terminate(id, false)));
    BOOST_TEST_REQUIRE(f.wait_for_one(id)->exited);
}

BOOST_AUTO_TEST_CASE(a_zero_timeout_does_not_wait_at_all)
{
    Fixture f;
    // 0 is the "tell me where things stand" spelling: the deadline is the
    // present, so the one pass the wait always takes IS the answer — even for a
    // child that would have exited on its own a moment later.
    const auto id = f.spawn_id(spec_for("sleep", {"0.2"}));
    const auto waited = f.run(f.store->wait_for_any({id}, 0));
    BOOST_TEST_REQUIRE(waited.snapshots.size() == std::size_t{1});
    BOOST_TEST(waited.timed_out);
    BOOST_TEST(!waited.finished);
    BOOST_TEST(!waited.snapshots.front().exited);
    BOOST_CHECK(waited.waited_milliseconds < 500);

    // And the session is exactly as it was: the look cost it nothing, and a
    // second call with a real deadline gets the exit.
    const auto later = f.wait_for_one(id);
    BOOST_TEST_REQUIRE(later.has_value());
    BOOST_TEST(later->exited);
    BOOST_TEST(later->result.execution.exit_code.value() == 0);
}

BOOST_AUTO_TEST_CASE(wait_for_any_ends_on_the_first_session_to_finish)
{
    // The property the set-general wait exists for: ONE child ending is what
    // ends the wait — the caller is not made to wait for the last of them, and
    // the deadline is not what answered. And the answer is still every session
    // in the selection, so the sibling that is still running comes back with
    // the one that ended, and comes back UNCHANGED.
    Fixture f;
    const auto slow = f.spawn_id(spec_for("sleep", {"30"}));
    const auto quick = f.spawn_id(spec_for("sh", {"-c", "sleep 0.3"}));

    const auto before = std::chrono::steady_clock::now();
    // No ids: the whole table, which is the same thing here.
    const auto waited = f.run(f.store->wait_for_any({}, 20000));
    const auto elapsed = std::chrono::steady_clock::now() - before;

    BOOST_TEST(waited.finished);
    BOOST_TEST(!waited.timed_out);
    // Ended by the quick child, so the deadline never fired: 20 s of waiting
    // would have shown up here.
    BOOST_CHECK(elapsed < std::chrono::seconds{10});
    BOOST_TEST_REQUIRE(waited.snapshots.size() == std::size_t{2});
    BOOST_TEST(waited.snapshots[0].id == slow);
    BOOST_TEST(!waited.snapshots[0].exited);
    BOOST_TEST(waited.snapshots[1].id == quick);
    BOOST_TEST(waited.snapshots[1].exited);
    BOOST_TEST(waited.snapshots[1].output_drained);

    // Nothing was released and nothing was killed: a wait only watches.
    BOOST_TEST(f.run(f.store->size()) == std::size_t{2});
    BOOST_TEST(!f.run(f.store->snapshot(slow))->exited);

    f.run(f.store->terminate(slow, false));
}

BOOST_AUTO_TEST_CASE(wait_for_any_over_nothing_answers_at_once)
{
    // A selection that resolves to nothing has no child whose ending could end
    // the wait, so holding the caller for the deadline would spend a timeout to
    // learn what it already knows. Both spellings of "nothing": an empty table,
    // and ids that name no session.
    Fixture f;
    const auto before = std::chrono::steady_clock::now();
    const auto empty = f.run(f.store->wait_for_any({}, 20000));
    const auto stale = f.run(f.store->wait_for_any({"proc_404"}, 20000));
    const auto elapsed = std::chrono::steady_clock::now() - before;

    BOOST_TEST(empty.snapshots.empty());
    BOOST_TEST(empty.timed_out);
    BOOST_TEST(!empty.finished);
    BOOST_TEST(stale.snapshots.empty());
    BOOST_TEST(stale.timed_out);
    BOOST_CHECK(elapsed < std::chrono::seconds{10});
}

BOOST_AUTO_TEST_CASE(waiting_returns_the_output_of_a_child_that_exits_at_once)
{
    // The race this case exists for: exited() is written by the handle's await
    // task the moment it observes the child, while the text the child already
    // printed may still be sitting in the pipe waiting for the read tasks. A
    // wait that watched only exited() returns an exit code of 0 with EMPTY
    // output here - which is exactly what a caller waiting for a command to
    // finish must not be told. So the wait watches output_drained() too.
    //
    // The spawn and the wait must run in ONE coroutine, with no hop back to
    // the test thread between them: that is what an agent turn looks like, and
    // the fixture's usual per-call co_spawn + future.get() round trip adds
    // enough latency for the readers to finish on their own - which HIDES the
    // race rather than testing it. (Checked: with the drain condition removed,
    // this case fails and the per-call form still passes.)
    //
    // Repeated because it is a race: one pass proves little, and a regression
    // would show up as an occasional empty capture.
    Fixture f;
    for (int attempt = 0; attempt < 20; ++attempt) {
        BOOST_TEST_CONTEXT("attempt " << attempt) {
            const auto waited = f.run(
                [&store = *f.store]() -> asio::awaitable<SessionSnapshot> {
                    // No initial wait: this case is about wait_for_any()
                    // doing the draining, so the spawn must not settle it.
                    auto spec = spec_for("echo", {"printed-and-gone"});
                    spec.initial_wait_timeout_milliseconds =
                        ProcessSessionStore::kNoInitialWait;
                    const auto spawned = co_await store.spawn(std::move(spec));
                    const auto outcome =
                        co_await store.wait_for_any({spawned.id}, 5000);
                    co_return outcome.snapshots.front();
                }());

            BOOST_TEST(waited.exited);
            BOOST_TEST(waited.output_drained);
            BOOST_TEST(waited.result.execution.exit_code.value() == 0);
            // The whole point: the exit code and the output are settled by
            // different tasks, and a caller must be given both.
            BOOST_TEST(waited.result.stdout_text.value() ==
                       "printed-and-gone\n");
            BOOST_TEST(f.run(f.store->release(waited.id)));
        }
    }
}

BOOST_AUTO_TEST_CASE(terminate_kills_and_graceful_asks)
{
    Fixture f;
    const auto killed = f.spawn_id(spec_for("sleep", {"30"}));
    BOOST_TEST(f.run(f.store->terminate(killed, false)));
    const auto killed_snapshot = f.wait_for_one(killed, 5000);
    BOOST_TEST_REQUIRE(killed_snapshot->exited);
    BOOST_TEST(killed_snapshot->result.execution.exit_code.value() == 9);

    const auto asked = f.spawn_id(spec_for("sleep", {"30"}));
    BOOST_TEST(f.run(f.store->terminate(asked, true)));
    const auto asked_snapshot = f.wait_for_one(asked, 5000);
    BOOST_TEST_REQUIRE(asked_snapshot->exited);
    // sleep does not catch SIGTERM, so it dies of the signal.
    BOOST_TEST(asked_snapshot->result.execution.exit_code.value() == 15);

    // Idempotent: an exited child answers false rather than failing.
    BOOST_TEST(!f.run(f.store->terminate(killed, false)));
}

BOOST_AUTO_TEST_CASE(release_refuses_a_running_child_and_retires_the_id)
{
    Fixture f;
    const auto live = f.spawn_id(spec_for("sleep", {"30"}));

    // Refused: releasing would drop the last reference to a live child and
    // its handle would kill it — a silent kill out of a bookkeeping call.
    BOOST_TEST(!f.run(f.store->release(live)));
    BOOST_TEST(f.run(f.store->size()) == std::size_t{1});
    BOOST_TEST(f.run(f.store->snapshot(live)).has_value());

    f.run(f.store->terminate(live, false));
    BOOST_TEST_REQUIRE(f.wait_for_one(live, 5000)->exited);

    BOOST_TEST(f.run(f.store->release(live)));
    BOOST_TEST(f.run(f.store->size()) == std::size_t{0});
    BOOST_TEST(!f.run(f.store->snapshot(live)).has_value());
    // A second release observes the missing entry and returns false.
    BOOST_TEST(!f.run(f.store->release(live)));

    // The id is SPENT. It does not come back — not for the next spawn, not for
    // any later one. An agent-facing id that reappeared would alias a process
    // the model still remembers from an earlier turn.
    const auto next = f.spawn_id(spec_for("true"));
    BOOST_TEST(next != live);
    const auto after = f.spawn_id(spec_for("true"));
    BOOST_TEST(after != next);
    BOOST_TEST(after != live);
}

BOOST_AUTO_TEST_CASE(a_concurrent_release_of_one_session_has_exactly_one_winner)
{
    // Eight callers share a deadline on three workers. The mutex makes the
    // exit check and removal one transaction: exactly one caller succeeds.
    MultiWorkerFixture f;
    constexpr int kRounds = 20;
    constexpr int kCallers = 8;

    for (int round = 0; round < kRounds; ++round) {
        BOOST_TEST_CONTEXT("round " << round) {
            const auto id = f.spawn_id(spec_for("true"));
            BOOST_TEST_REQUIRE(f.wait_for_one(id, 5000)->exited);

            const auto gate_at = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds{20};
            std::vector<std::future<bool>> releases;
            releases.reserve(kCallers);
            for (int caller = 0; caller < kCallers; ++caller) {
                releases.push_back(asio::co_spawn(
                    f.io,
                    [&store = *f.store, id, gate_at]() -> asio::awaitable<bool> {
                        // The barrier: every caller waits on the same instant,
                        // so none of them can finish before the last one has
                        // started.
                        asio::steady_timer gate{
                            co_await asio::this_coro::executor};
                        gate.expires_at(gate_at);
                        co_await gate.async_wait(asio::use_awaitable);
                        co_return co_await store.release(id);
                    },
                    asio::use_future));
            }
            int released = 0;
            for (auto& release : releases) {
                if (release.get()) ++released;
            }
            BOOST_TEST(released == 1);
            BOOST_TEST(f.run(f.store->size()) == std::size_t{0});
        }
    }
}

BOOST_AUTO_TEST_CASE(a_descendant_holding_the_pipes_delays_output_completion)
{
    // The two facts SessionSnapshot carries, in the case where they come
    // apart for a reason that has nothing to do with scheduling: the direct
    // child exits at once, but something it started inherited its stdout and
    // stderr and is still holding them open, so the capture cannot complete
    // until that descendant is gone. The exit code is readable the whole time.
    //
    // `sh -c 'sleep N & exit 0'` is the smallest way to arrange it: the shell
    // puts the sleep in the background (it inherits the pipes) and exits
    // without waiting for it.
    Fixture f;
    const auto id = f.spawn_id(spec_for("sh", {"-c", "sleep 2 & exit 0"}));

    const auto waited = f.run(f.store->wait_for_any({id}, 500));
    BOOST_TEST_REQUIRE(waited.snapshots.size() == std::size_t{1});
    // The child is gone, and it ended successfully...
    BOOST_TEST(waited.snapshots.front().exited);
    BOOST_TEST(waited.snapshots.front().result.execution.exit_code.value() == 0);
    // ...while the capture is NOT finished, so the session is not FINISHED
    // either and nothing has ended the wait. This is the state a caller must be
    // able to see: read on `exited` alone, it would take what it has for all
    // the output there is, and it has none.
    BOOST_TEST(!waited.snapshots.front().output_drained);
    BOOST_TEST(!waited.finished);
    BOOST_TEST(waited.timed_out);

    // And the flag is a fact about the pipes, not a verdict about the session:
    // when the descendant goes, the capture finishes, the session counts as
    // finished, and the same session reports it.
    const auto finished = f.run(f.store->wait_for_any({id}, 10000));
    BOOST_TEST_REQUIRE(finished.snapshots.size() == std::size_t{1});
    BOOST_TEST(finished.finished);
    BOOST_TEST(!finished.timed_out);
    BOOST_TEST(finished.snapshots.front().exited);
    BOOST_TEST(finished.snapshots.front().output_drained);
    BOOST_TEST(f.run(f.store->release(id)));
}

BOOST_AUTO_TEST_CASE(snapshots_lists_every_session_in_id_order)
{
    Fixture f;
    const auto first = f.spawn_id(spec_for("sleep", {"5"}, "first"));
    const auto second = f.spawn_id(spec_for("sleep", {"5"}, "second"));

    const auto all = f.run(f.store->snapshots());
    BOOST_TEST_REQUIRE(all.size() == std::size_t{2});
    // Sorted by id, so a listing reads the same way across turns instead of
    // in hash order.
    BOOST_TEST(all[0].id == first);
    BOOST_TEST(all[1].id == second);
    BOOST_TEST(all[0].result.spec.description == std::string("first"));

    // A named subset, and an id naming nothing is skipped rather than
    // reported: the returned list IS the answer to "which of these exist".
    const auto some = f.run(f.store->snapshots({second, "proc_404"}));
    BOOST_TEST_REQUIRE(some.size() == std::size_t{1});
    BOOST_TEST(some[0].id == second);

    f.run(f.store->terminate(first, false));
    f.run(f.store->terminate(second, false));
}

BOOST_AUTO_TEST_CASE(output_truncation_is_reported)
{
    Fixture f;
    process::LaunchSpec spec = spec_for("seq", {"1", "20000"});
    spec.max_output_bytes = 64;
    const auto id = f.spawn_id(std::move(spec));
    BOOST_TEST_REQUIRE(f.wait_for_one(id, 10000)->exited);

    const auto read = f.run(f.store->read_output(id, OutputStream::Stdout, true));
    BOOST_TEST_REQUIRE(read.has_value());
    // The capture stops at the cap; the child still ran to completion,
    // because the handle keeps draining what it discards.
    BOOST_TEST(read->standard_output.text.size() == std::size_t{64});
    BOOST_TEST(read->standard_output.truncated);
}

BOOST_AUTO_TEST_CASE(destroying_the_store_does_not_leak_running_children)
{
    // The last-resort tail: a host that forgot shutdown() must still not
    // leak children. Dropping the table is not enough on its own — each
    // handle's await task holds its handle alive, so the destructor signals
    // the recorded pids itself.
    Fixture f;
    const auto id = f.spawn_id(spec_for("sleep", {"60"}));
    const pid_t pid = f.run(f.store->snapshot(id))->result.spec.pid;
    BOOST_TEST_REQUIRE(pid > 0);
    BOOST_TEST(process_running(pid));

    f.shutdown();
    BOOST_TEST(wait_until_not_running(pid));
}

BOOST_AUTO_TEST_CASE(destroying_the_store_under_several_workers_still_reaps_children)
{
    // The same last-resort tail, on the fixture whose context runs on THREE
    // worker threads, so the table being dropped is one whose strands and
    // handles have really been spread across threads.
    //
    // What makes the tail answerable is the pair the destructor reads: the pid
    // copied at spawn (written once, before the handle was shared with anyone)
    // and exited(), the handle's one thread-safe observation. It reads both
    // WITHOUT a strand hop, because a destructor cannot make one — and the
    // answer matters: a child that was observed was also reaped, so its pid may
    // already belong to somebody else.
    //
    // Quiesce the context before raw-pid cleanup to avoid racing child reaping.
    MultiWorkerFixture f;
    const auto id = f.spawn_id(spec_for("sleep", {"60"}));
    const pid_t pid = f.run(f.store->snapshot(id))->result.spec.pid;
    BOOST_TEST_REQUIRE(pid > 0);
    BOOST_TEST(process_running(pid));

    f.shutdown();
    BOOST_TEST(wait_until_not_running(pid));
}

BOOST_AUTO_TEST_CASE(terminate_all_ends_every_live_child)
{
    // Signal every live child and report how many, retaining readable
    // sessions. This operation does not provide the shutdown() lifetime fence.
    Fixture f;
    const auto first = f.spawn_id(spec_for("sleep", {"30"}));
    const auto second = f.spawn_id(spec_for("sleep", {"30"}));
    const auto done = f.spawn_id(spec_for("true"));
    BOOST_TEST_REQUIRE(f.wait_for_one(done, 5000)->exited);

    // Two live children signalled; the one that already exited is not
    // counted, and is not an error.
    BOOST_TEST(f.run(f.store->terminate_all(false)) == std::size_t{2});

    BOOST_TEST_REQUIRE(f.wait_for_one(first, 5000)->exited);
    BOOST_TEST_REQUIRE(f.wait_for_one(second, 5000)->exited);
    // The sessions survive the sweep, so their output is still collectable —
    // terminate_all ends children, it does not forget them.
    BOOST_TEST(f.run(f.store->size()) == std::size_t{3});
    BOOST_TEST(f.run(f.store->read_output(first, OutputStream::Both, true))
                   .has_value());

    // Idempotent: nothing is left alive to signal.
    BOOST_TEST(f.run(f.store->terminate_all(false)) == std::size_t{0});
}

BOOST_AUTO_TEST_CASE(store_operations_can_be_awaited_from_another_context)
{
    MultiWorkerFixture f;
    asio::io_context caller;
    auto result = asio::co_spawn(
        caller,
        [&]() -> asio::awaitable<void> {
            auto spec = spec_for("cat");
            spec.initial_wait_timeout_milliseconds = 10;
            const auto spawned = co_await f.store->spawn(std::move(spec));
            BOOST_TEST(!spawned.finished);
            BOOST_TEST(co_await f.store->write_input(spawned.id, "hello\n", true));
            const std::vector<std::string> ids{spawned.id};
            const auto waited = co_await f.store->wait_for_any(ids, 5000);
            BOOST_TEST_REQUIRE(waited.finished);
            const auto output = co_await f.store->read_output(spawned.id, OutputStream::Both, false);
            BOOST_TEST_REQUIRE(output.has_value());
            BOOST_TEST(output->standard_output.text == "hello\n");
            const auto empty = co_await f.store->read_output(spawned.id, OutputStream::Both, false);
            BOOST_TEST_REQUIRE(empty.has_value());
            BOOST_TEST(empty->standard_output.text.empty());
            BOOST_TEST(co_await f.store->release(spawned.id));

            auto sleeper = spec_for("sleep", {"60"});
            sleeper.initial_wait_timeout_milliseconds = 10;
            const auto live = co_await f.store->spawn(std::move(sleeper));
            BOOST_TEST(co_await f.store->terminate(live.id, false));
            const std::vector<std::string> live_ids{live.id};
            const auto stopped = co_await f.store->wait_for_any(live_ids, 5000);
            BOOST_TEST(stopped.finished);
            BOOST_TEST(co_await f.store->release(live.id));
        },
        asio::use_future
    );
    caller.run();
    result.get();
}

BOOST_AUTO_TEST_CASE(concurrent_spawns_reserve_capacity_and_failed_launches_return_it)
{
    MultiWorkerFixture f;
    f.store = std::make_shared<ProcessSessionStore>(f.io.get_executor(), 1);
    BOOST_CHECK_THROW(
        f.run(f.store->spawn(spec_for("/nonexistent/simplex-process"))),
        process::ProcessException
    );

    constexpr int callers = 16;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{50};
    std::vector<std::future<bool>> launches;
    for (int i = 0; i < callers; ++i) {
        launches.push_back(asio::co_spawn(
            f.io,
            [store = f.store, deadline]() -> asio::awaitable<bool> {
                asio::steady_timer timer{co_await asio::this_coro::executor};
                timer.expires_at(deadline);
                co_await timer.async_wait(asio::use_awaitable);
                auto spec = spec_for("true");
                spec.initial_wait_timeout_milliseconds = 100;
                try {
                    co_await store->spawn(std::move(spec));
                    co_return true;
                } catch (const std::runtime_error&) {
                    co_return false;
                }
            },
            asio::use_future
        ));
    }
    int accepted = 0;
    for (auto& launch : launches) {
        if (launch.get()) {
            ++accepted;
        }
    }
    BOOST_TEST(accepted == 1);
    BOOST_TEST(f.run(f.store->size()) == std::size_t{1});
    const auto waited = f.run(f.store->wait_for_any({}, 5000));
    BOOST_TEST_REQUIRE(waited.finished);
    BOOST_TEST(f.run(f.store->release(waited.snapshots.front().id)));
    // Releasing the retained entry makes the same capacity available again.
    auto next = spec_for("true");
    next.initial_wait_timeout_milliseconds = 5000;
    BOOST_TEST(f.run(f.store->spawn(std::move(next))).finished);
}

BOOST_AUTO_TEST_CASE(live_output_reads_and_snapshots_are_serialized_across_contexts)
{
    MultiWorkerFixture f;
    asio::io_context caller;
    const auto id = f.spawn_id(spec_for(
        "sh", {"-c", "while IFS= read -r line; do "
                     "printf '%s\\n' \"$line\"; "
                     "printf '%s\\n' \"$line\" >&2; done"}
    ));

    struct Chunk {
        std::size_t end;
        std::string text;
    };
    struct Capture {
        std::vector<Chunk> out;
        std::vector<Chunk> err;
        std::size_t live_reads = 0;
    };

    constexpr int rounds = 32;
    std::vector<std::string> messages;
    std::string expected;
    for (int i = 0; i < rounds; ++i) {
        messages.push_back(std::format("{}:{}\n", i, std::string(512, 'a' + i % 26)));
        expected += messages.back();
    }

    // Only caller.run() accesses these counters, on this test's thread.
    // The store's three workers concurrently drain the child's output pipes.
    std::size_t consumed_out = 0;
    std::size_t consumed_err = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds{10};
    std::vector<std::future<Capture>> readers;
    for (int reader = 0; reader < 2; ++reader) {
        readers.push_back(asio::co_spawn(
            caller,
            [&]() -> asio::awaitable<Capture> {
                Capture captured;
                asio::steady_timer timer{co_await asio::this_coro::executor};
                for (;;) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        throw std::runtime_error("live output reader timed out");
                    }
                    const auto snapshot = co_await f.store->snapshot(id);
                    BOOST_TEST_REQUIRE(snapshot.has_value());
                    const auto& output = snapshot->result;
                    BOOST_TEST(expected.starts_with(output.stdout_text.value_or("")));
                    BOOST_TEST(expected.starts_with(output.stderr_text.value_or("")));

                    const auto delta = co_await f.store->read_output(
                        id, OutputStream::Both, false
                    );
                    BOOST_TEST_REQUIRE(delta.has_value());
                    if (!delta->standard_output.text.empty()) {
                        captured.out.push_back({
                            delta->stdout_cursor, delta->standard_output.text
                        });
                        consumed_out += delta->standard_output.text.size();
                        if (!snapshot->exited) {
                            ++captured.live_reads;
                        }
                    }
                    if (!delta->standard_error.text.empty()) {
                        captured.err.push_back({
                            delta->stderr_cursor, delta->standard_error.text
                        });
                        consumed_err += delta->standard_error.text.size();
                    }
                    if (snapshot->exited && snapshot->output_drained) {
                        co_return captured;
                    }
                    timer.expires_after(std::chrono::milliseconds{1});
                    co_await timer.async_wait(asio::use_awaitable);
                }
            },
            asio::use_future
        ));
    }
    auto writer = asio::co_spawn(
        caller,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer timer{co_await asio::this_coro::executor};
            std::size_t sent = 0;
            for (const auto& message : messages) {
                BOOST_TEST(co_await f.store->write_input(id, message, false));
                sent += message.size();
                // Do not let the child exit before readers observe live output.
                // Each round waits for both streams, without assuming pipe read
                // boundaries or which competing delta reader consumes a chunk.
                while (consumed_out < sent || consumed_err < sent) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        throw std::runtime_error("live output writer timed out");
                    }
                    timer.expires_after(std::chrono::milliseconds{1});
                    co_await timer.async_wait(asio::use_awaitable);
                }
            }
            BOOST_TEST(co_await f.store->write_input(id, "", true));
        },
        asio::use_future
    );
    caller.run();
    writer.get();

    Capture combined;
    for (auto& reader : readers) {
        auto captured = reader.get();
        combined.out.insert(
            combined.out.end(), captured.out.begin(), captured.out.end()
        );
        combined.err.insert(
            combined.err.end(), captured.err.begin(), captured.err.end()
        );
        combined.live_reads += captured.live_reads;
    }
    BOOST_TEST(combined.live_reads >= std::size_t{rounds});
    auto verify = [&](std::vector<Chunk>& chunks) {
        std::sort(chunks.begin(), chunks.end(), [](const Chunk& a, const Chunk& b) {
            return a.end < b.end;
        });
        std::string actual;
        for (const auto& chunk : chunks) {
            actual += chunk.text;
            // Contiguous cursor ranges prove that competing readers neither
            // duplicate nor skip bytes, even when reads complete out of order.
            BOOST_TEST(chunk.end == actual.size());
        }
        BOOST_TEST(actual == expected);
    };
    verify(combined.out);
    verify(combined.err);
    BOOST_TEST(f.run(f.store->release(id)));
}

BOOST_AUTO_TEST_CASE(store_incarnations_never_alias_historical_ids)
{
    Fixture first;
    Fixture second;
    process::LaunchSpec spec;
    spec.executable = "echo";
    spec.arguments = {"first"};
    spec.initial_wait_timeout_milliseconds = 1000;
    const auto old = first.run(first.store->spawn(spec)).id;
    spec.arguments = {"second"};
    const auto current = second.run(second.store->spawn(spec)).id;
    BOOST_TEST(old != current);
    BOOST_TEST(!second.run(second.store->snapshot(old)).has_value());
    BOOST_TEST(!second.run(second.store->write_input(old, "bad", false)));
    BOOST_TEST(!second.run(second.store->terminate(old, false)));
    BOOST_TEST(!second.run(second.store->release(old)));
    BOOST_TEST(second.run(second.store->snapshot(current)).has_value());
    first.run(first.store->shutdown());
    second.run(second.store->shutdown());
}
