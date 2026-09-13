#define BOOST_TEST_MODULE ProcessSessionStoreTests
#include <boost/test/unit_test.hpp>

#include "tools/intrinsic/process/session_store.hpp"

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
// child, waiting with and without a deadline, the difference between "the
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

    /// Stop the context and join its thread FIRST, then drop the store.
    ///
    /// That order is what ThreadSanitizer found the other way round. Killing a
    /// leaked child is a synchronous ::kill on a pid the spawn recorded, so the
    /// destructor needs no executor at all — but the table's own strand shares
    /// refcounted state with the operations still queued on the context, and
    /// freeing it from the test thread while a worker is finishing one is a
    /// data race on that shared state (asio's executor refcount), which TSan
    /// reports as a race in `operator delete`. Nothing that shares an executor
    /// with a running context should be destroyed before it is quiesced.
    ///
    /// Idempotent, so a case may call it early to observe what teardown does.
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

/// The same store on a context with SEVERAL worker threads.
///
/// The single-runner fixture above is what a case wants when it is testing
/// behavior, and it is useless for testing strand discipline: with one thread
/// nothing is ever concurrent, so an off-strand read cannot be observed no
/// matter how wrong it is. This fixture is the one that gives the two-level
/// strand model something to actually do — the store's strand and a session's
/// strand are on different threads often enough to matter.
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

    /// Stop the context, join every worker, and only THEN drop the store —
    /// the order a host that never called terminate_all() leaves behind, and
    /// the only order that is free of races: the tail's signal is a synchronous
    /// ::kill on a recorded pid, which needs no executor, while the table's
    /// strand shares refcounted state with the operations still queued on the
    /// context. Freeing it before the workers are joined is a data race in that
    /// refcount, which ThreadSanitizer reports as a race in operator delete.
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

    // Ids are minted as readable names, starting at 1.
    BOOST_TEST(id == std::string("proc_1"));

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
    // drain for the same reason wait_for_exit() does.
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
    // the store's strand BEFORE the wait suspends, so a concurrent caller sees
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
    // still gets proc_1.
    BOOST_TEST(f.run(f.store->size()) == std::size_t{0});
    const auto id = f.spawn_id(spec_for("true"));
    BOOST_TEST(id == std::string("proc_1"));
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
    const auto waited = f.run(f.store->wait_for_exit(id, 5000));
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
    BOOST_TEST_REQUIRE(f.run(f.store->wait_for_exit(id, 5000))->exited);

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
    BOOST_TEST(!f.run(f.store->wait_for_exit("proc_404", 10)).has_value());
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

    const auto waited = f.run(f.store->wait_for_exit(id, 5000));
    BOOST_TEST_REQUIRE(waited.has_value());
    BOOST_TEST(waited->exited);
    BOOST_TEST(waited->result.execution.exit_code.value() == 0);

    const auto read = f.run(f.store->read_output(id, OutputStream::Stdout, true));
    BOOST_TEST_REQUIRE(read.has_value());
    BOOST_TEST(read->standard_output.text ==
               std::string("line one\nline two\n"));
}

BOOST_AUTO_TEST_CASE(wait_returns_early_when_the_deadline_fires)
{
    Fixture f;
    const auto id = f.spawn_id(spec_for("sleep", {"30"}));

    const auto before = std::chrono::steady_clock::now();
    const auto waited = f.run(f.store->wait_for_exit(id, 150));
    const auto elapsed = std::chrono::steady_clock::now() - before;

    BOOST_TEST_REQUIRE(waited.has_value());
    // A timeout is a result, not a failure: the child is reported as still
    // running, and it really is.
    BOOST_TEST(!waited->exited);
    BOOST_CHECK(waited->result.execution.state == process::ProcessState::Running);
    BOOST_CHECK(elapsed < std::chrono::seconds{5});

    // Still waitable afterwards — the deadline ended the wait, not the child.
    BOOST_TEST(f.run(f.store->terminate(id, false)));
    BOOST_TEST_REQUIRE(f.run(f.store->wait_for_exit(id, 5000))->exited);
}

BOOST_AUTO_TEST_CASE(a_zero_timeout_waits_for_the_child)
{
    Fixture f;
    // 0 disables the deadline: the wait ends on the exit, however long that
    // takes (here, a child that exits on its own shortly).
    const auto id = f.spawn_id(spec_for("sleep", {"0.2"}));
    const auto waited = f.run(f.store->wait_for_exit(id, 0));
    BOOST_TEST_REQUIRE(waited.has_value());
    BOOST_TEST(waited->exited);
    BOOST_TEST(waited->result.execution.exit_code.value() == 0);
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
                [&store = *f.store]()
                    -> asio::awaitable<std::optional<SessionSnapshot>> {
                    // No initial wait: this case is about wait_for_exit()
                    // doing the draining, so the spawn must not settle it.
                    auto spec = spec_for("echo", {"printed-and-gone"});
                    spec.initial_wait_timeout_milliseconds =
                        ProcessSessionStore::kNoInitialWait;
                    const auto spawned = co_await store.spawn(std::move(spec));
                    co_return co_await store.wait_for_exit(spawned.id, 5000);
                }());

            BOOST_TEST_REQUIRE(waited.has_value());
            BOOST_TEST(waited->exited);
            BOOST_TEST(waited->result.execution.exit_code.value() == 0);
            // The whole point: the exit code and the output are settled by
            // different tasks, and a caller must be given both.
            BOOST_TEST(waited->result.stdout_text.value() ==
                       "printed-and-gone\n");
            BOOST_TEST(f.run(f.store->release(waited->id)));
        }
    }
}

BOOST_AUTO_TEST_CASE(terminate_kills_and_graceful_asks)
{
    Fixture f;
    const auto killed = f.spawn_id(spec_for("sleep", {"30"}));
    BOOST_TEST(f.run(f.store->terminate(killed, false)));
    const auto killed_snapshot = f.run(f.store->wait_for_exit(killed, 5000));
    BOOST_TEST_REQUIRE(killed_snapshot->exited);
    BOOST_TEST(killed_snapshot->result.execution.exit_code.value() == 9);

    const auto asked = f.spawn_id(spec_for("sleep", {"30"}));
    BOOST_TEST(f.run(f.store->terminate(asked, true)));
    const auto asked_snapshot = f.run(f.store->wait_for_exit(asked, 5000));
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
    BOOST_TEST_REQUIRE(f.run(f.store->wait_for_exit(live, 5000))->exited);

    BOOST_TEST(f.run(f.store->release(live)));
    BOOST_TEST(f.run(f.store->size()) == std::size_t{0});
    BOOST_TEST(!f.run(f.store->snapshot(live)).has_value());
    // A second release of the same id is false, not a second success: the
    // removal happens after a strand hop, so the id is looked up again when
    // the coroutine gets back and "already gone" has to be answered honestly.
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
    // release() reads the child's state on the session's strand and removes the
    // entry on the store's strand, so callers racing for one session must not
    // both report success. This case puts eight callers inside release() at the
    // same instant, on a context with three worker threads, and asserts the
    // contract that has to hold for all of them: exactly one removes the
    // session, the rest answer false, and the table ends empty.
    //
    // The callers are held at a SHARED DEADLINE rather than started in a loop,
    // because a loop is not a race at all: each release is a few strand hops and
    // the next caller only starts a moment later, so a loop tests the sequential
    // path twice and calls it concurrency.
    //
    // WHAT THIS CASE DOES NOT DO, stated because the difference matters when
    // reading a mutation report: it does not drive the re-lookup that release()
    // performs after its second hop. Every hop in this store is a continuation
    // of the handler before it, so on a free worker the first caller's three
    // hops run back to back before another caller's first hop is even dequeued
    // — the losers here find the session already gone, which is the FIRST
    // lookup answering. Measured, rather than assumed: with the re-lookup
    // deleted, 120 rounds of this case still never produced a second winner. So
    // that check is defensive (it costs one map lookup and turns a "cannot
    // happen" into an answer), and the assertions below are about the property
    // a host actually depends on.
    MultiWorkerFixture f;
    constexpr int kRounds = 20;
    constexpr int kCallers = 8;

    for (int round = 0; round < kRounds; ++round) {
        BOOST_TEST_CONTEXT("round " << round) {
            const auto id = f.spawn_id(spec_for("true"));
            BOOST_TEST_REQUIRE(f.run(f.store->wait_for_exit(id, 5000))->exited);

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

    const auto waited = f.run(f.store->wait_for_exit(id, 500));
    BOOST_TEST_REQUIRE(waited.has_value());
    // The child is gone, and it ended successfully...
    BOOST_TEST(waited->exited);
    BOOST_TEST(waited->result.execution.exit_code.value() == 0);
    // ...while the capture is NOT finished. This is the state a caller must be
    // able to see: read on `exited` alone, it would take what it has for all
    // the output there is, and it has none.
    BOOST_TEST(!waited->output_drained);

    // And the flag is a fact about the pipes, not a verdict about the session:
    // when the descendant goes, the capture finishes and the same session
    // reports it.
    const auto finished = f.run(f.store->wait_for_exit(id, 10000));
    BOOST_TEST_REQUIRE(finished.has_value());
    BOOST_TEST(finished->exited);
    BOOST_TEST(finished->output_drained);
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
    BOOST_TEST_REQUIRE(f.run(f.store->wait_for_exit(id, 10000))->exited);

    const auto read = f.run(f.store->read_output(id, OutputStream::Stdout, true));
    BOOST_TEST_REQUIRE(read.has_value());
    // The capture stops at the cap; the child still ran to completion,
    // because the handle keeps draining what it discards.
    BOOST_TEST(read->standard_output.text.size() == std::size_t{64});
    BOOST_TEST(read->standard_output.truncated);
}

BOOST_AUTO_TEST_CASE(destroying_the_store_does_not_leak_running_children)
{
    // The last-resort tail: a host that forgot terminate_all() must still not
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
    // The teardown ORDER is part of the case rather than incidental: the
    // context is stopped and every worker joined before the table is dropped
    // (see MultiWorkerFixture::shutdown). That is not tidiness — the table's
    // strand shares refcounted state with the operations queued on the context,
    // and freeing it while a worker is still finishing one is a race that
    // ThreadSanitizer reports in operator delete. The tail itself needs no
    // executor, so nothing is lost by quiescing first.
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
    // The shutdown path a host is meant to use: it signals every live child
    // and reports how many, while their sessions stay readable.
    Fixture f;
    const auto first = f.spawn_id(spec_for("sleep", {"30"}));
    const auto second = f.spawn_id(spec_for("sleep", {"30"}));
    const auto done = f.spawn_id(spec_for("true"));
    BOOST_TEST_REQUIRE(f.run(f.store->wait_for_exit(done, 5000))->exited);

    // Two live children signalled; the one that already exited is not
    // counted, and is not an error.
    BOOST_TEST(f.run(f.store->terminate_all(false)) == std::size_t{2});

    BOOST_TEST_REQUIRE(f.run(f.store->wait_for_exit(first, 5000))->exited);
    BOOST_TEST_REQUIRE(f.run(f.store->wait_for_exit(second, 5000))->exited);
    // The sessions survive the sweep, so their output is still collectable —
    // terminate_all ends children, it does not forget them.
    BOOST_TEST(f.run(f.store->size()) == std::size_t{3});
    BOOST_TEST(f.run(f.store->read_output(first, OutputStream::Both, true))
                   .has_value());

    // Idempotent: nothing is left alive to signal.
    BOOST_TEST(f.run(f.store->terminate_all(false)) == std::size_t{0});
}
