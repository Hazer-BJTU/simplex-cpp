#include "tools/intrinsic/process/session_store.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>

#include <cerrno>
#include <csignal>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "logging/logger.hpp"

namespace tools::intrinsic {
namespace {

// How often wait_for_exit() re-reads the handle's observation. Matched to
// ProcessHandle's own probe cadence: the await task there notices a missed
// exit within one such interval, so watching faster would only spin.
constexpr auto kExitPollInterval = std::chrono::milliseconds{50};

} // namespace

ProcessSessionStore::ProcessSessionStore(
    boost::asio::any_io_executor executor,
    std::size_t max_sessions
): _strand(boost::asio::make_strand(executor)),
   _executor(std::move(executor)),
   _max_sessions(max_sessions)
{}

ProcessSessionStore::~ProcessSessionStore()
{
    // The last-resort tail, not the shutdown mechanism — terminate_all() is
    // (see the header). Dropping the table is NOT enough on its own: each
    // handle's await task holds a reference to its own handle until the
    // child's terminal state is observed, so this destructor's release is
    // rarely the last one, the handle's own destructor does not run, and the
    // child would keep running with nobody left to manage it.
    //
    // So the signal is sent HERE, synchronously, with ::kill on the pid the
    // spawn recorded — a destructor cannot await the handle's own coroutine
    // terminate(). SIGKILL rather than SIGTERM: this path is reached when a
    // host forgot to shut down, and a polite signal a nobody is watching for
    // would leave the child running anyway.
    //
    // The two reads below are the two that a destructor is allowed to make,
    // and the choice is deliberate rather than convenient:
    //
    //   the pid     copied into the Session at spawn, so this runs off the
    //               handle entirely — and the value was written once, before
    //               the handle was shared with anyone.
    //   exited()    the handle's one thread-safe observation (a latch, see
    //               process_handle.hpp). It has to be: the question this path
    //               asks is "was this child already observed?", and a child
    //               that was observed was also REAPED, so its pid may already
    //               belong to somebody else. Killing on a stale pid is the one
    //               failure here that hurts a stranger, so it is answered
    //               properly even though a destructor cannot hop to a strand.
    //               The race this replaces — reading strand-owned state from
    //               the wrong thread and calling the answer luck — is what the
    //               latch exists to remove.
    for (const auto& [id, session] : _sessions) {
        if (!session->handle || session->handle->exited() || session->pid <= 0) {
            continue;
        }
        const pid_t pid = session->pid;
        logging::Logger::warning(std::format(
            "process session store destroyed while session {} (pid {}) is "
            "still running; killing it. A host should await terminate_all() "
            "before dropping the store",
            id, static_cast<int>(pid)));
        if (::kill(pid, SIGKILL) != 0 && errno != ESRCH) {
            logging::Logger::error(std::format(
                "killing session {} (pid {}) during store destruction failed: "
                "errno {}", id, static_cast<int>(pid), errno));
        }
    }
    _sessions.clear();
}

// ---- id allocation ----------------------------------------------------------

ProcessSessionStore::SessionId ProcessSessionStore::mint_id_on_strand()
{
    // Monotonic, always. No pool, no reuse: an agent-facing id that came back
    // around would alias a process the model remembers from an earlier turn
    // (the header's identity note). The cost of never reusing is a number
    // that grows; the cost of reuse is killing the wrong process.
    return std::format("proc_{}", _next_id++);
}

ProcessSessionStore::SessionPtr ProcessSessionStore::find_on_strand(
    const SessionId& id) const
{
    const auto found = _sessions.find(id);
    if (found == _sessions.end()) {
        return nullptr;
    }
    return found->second;
}

// ---- spawn ------------------------------------------------------------------

boost::asio::awaitable<SpawnResult>
ProcessSessionStore::spawn(process::LaunchSpec spec)
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);

    if (_sessions.size() >= _max_sessions) {
        throw std::runtime_error(std::format(
            "the process session store is full: {} of {} sessions are in use. "
            "Release an exited session before spawning another",
            _sessions.size(), _max_sessions));
    }

    // The caller's initial-wait window is HONOURED as the spec carries it —
    // that is what lets one spawn serve both `ls` and a server, and it is what
    // ProcessHandle::await_initial_execution() was designed to do.
    //
    // Detach is the one field forced, because the alternative is a leak rather
    // than a policy: a session is a child a caller can come back to, so a
    // store that killed a child at the end of its window would be handing out
    // ids for processes it had just destroyed. A caller that wants the child
    // dead calls terminate().
    spec.detach_on_timeout = true;
    const std::uint64_t window = spec.initial_wait_timeout_milliseconds;

    // One strand per child, derived from the store's executor: two children
    // have no reason to queue behind each other (header, threading).
    auto session_strand = boost::asio::make_strand(_executor);

    // Construction IS the spawn, and it throws ProcessException on failure —
    // passed through untranslated, since its stage and launch context are
    // exactly what a caller needs. Nothing has been registered at this point,
    // so a failed launch leaves the table untouched.
    auto handle = std::make_shared<process::ProcessHandle>(
        std::move(spec), session_strand);

    auto session = std::make_shared<Session>(Session{
        .id = mint_id_on_strand(),
        .handle = handle,
        .strand = session_strand,
        // Read here, before the handle is shared with any task: the pid is
        // stamped by the constructor and never written again, and this copy is
        // what lets the destructor signal the child without touching the
        // handle at all (the header's shutdown note).
        .pid = handle->pid(),
        .stdout_cursor = 0,
        .stderr_cursor = 0,
    });

    // The first half of the handle's lifecycle contract.
    co_await handle->start_background_io_tasks();

    // REGISTERED BEFORE THE WAIT, and that ordering is the point of this
    // function's shape. The wait below suspends for up to the whole window, and
    // during it the session must already be findable: a concurrent poll has to
    // see the child that is running, and — for a window that outlives the
    // launch — the id this call is about to return must not be mintable twice.
    // Everything touching _sessions therefore happens here, on the store's
    // strand, before anything suspends on the child.
    const SessionId id = session->id;
    _sessions.emplace(id, session);
    logging::Logger::debug(std::format(
        "process session {} spawned (pid {}), initial wait {} ms", id,
        static_cast<int>(handle->pid()), window));

    if (window == kNoInitialWait) {
        // No wait requested: drive the handle to its terminal observation in
        // the background and answer with the id. The await task is DETACHED and
        // holds the handle alive by shared_ptr, so the observation still
        // happens even if the session is released meanwhile — which is what the
        // handle's contract demands (an unobserved handle keeps itself alive
        // forever).
        boost::asio::co_spawn(
            session_strand,
            [handle]() -> boost::asio::awaitable<void> {
                // With the deadline disabled the bool is always true and says
                // nothing; the side effect is what matters — the handle
                // observes and records its child's terminal state, which is
                // what exited() and status() report from here on.
                co_await handle->await_initial_execution();
            },
            boost::asio::detached);
        co_return SpawnResult{.id = id, .finished = false, .output_drained = false};
    }

    // The window applies: race it against the child, which is precisely what
    // await_initial_execution() does. On expiry it detaches (forced above) and
    // restarts the await task ITSELF, so the aftermath is still recorded and
    // there is nothing to co_spawn here for that case.
    //
    // This suspends onto the SESSION's strand and stays there, so _sessions is
    // untouchable from here on — already handled above. The hop is stated
    // explicitly below rather than left to the propagation rules, because the
    // code that follows reads the handle's strand-owned output state.
    const bool finished = co_await handle->await_initial_execution();
    co_await boost::asio::dispatch(session_strand, boost::asio::use_awaitable);

    if (finished) {
        // Terminal, but the output may still be in flight: the await task
        // records the exit as soon as it observes the child, while the readers
        // are still draining the pipes. Answering "finished" without waiting
        // for that would let a caller report an exit code with empty output —
        // the race process/process_handle.hpp's output_drained() exists for,
        // and the one wait_for_exit() waits out for the same reason.
        //
        // Bounded by the window that has already been granted: a child whose
        // output somehow never drains must not turn a finished spawn into a
        // hang.
        boost::asio::steady_timer drain{session_strand};
        const auto drain_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds{window};
        while (!handle->output_drained() &&
               std::chrono::steady_clock::now() < drain_deadline) {
            drain.expires_after(kExitPollInterval);
            co_await drain.async_wait(boost::asio::use_awaitable);
        }
    }

    // `finished` and `output_drained` are reported SEPARATELY, and this is the
    // one place in the store where they can disagree: the child exited inside
    // the window, while a descendant it left behind holds the inherited pipes
    // open, so the capture is still incomplete when the bound above fires. The
    // caller gets both facts rather than one bool that would have to mean two
    // things (SpawnResult).
    co_return SpawnResult{
        .id = id,
        .finished = finished,
        .output_drained = finished && handle->output_drained(),
    };
}

// ---- observation ------------------------------------------------------------

boost::asio::awaitable<SessionSnapshot>
ProcessSessionStore::snapshot_on_session(SessionPtr session)
{
    // snapshot() and exited() read strand-owned state, so the read happens
    // on the session's own strand and what leaves is a value.
    co_await boost::asio::dispatch(session->strand, boost::asio::use_awaitable);
    co_return SessionSnapshot{
        .id = session->id,
        .result = session->handle->snapshot(),
        .exited = session->handle->exited(),
        // Both facts, always: a caller that only ever sees `exited` cannot
        // tell a complete capture from one whose pipes somebody else still
        // holds open (SessionSnapshot).
        .output_drained = session->handle->output_drained(),
    };
}

boost::asio::awaitable<std::vector<SessionSnapshot>>
ProcessSessionStore::snapshots(std::vector<SessionId> ids) const
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);

    // Collected first, on the store's strand, so the table is not read again
    // after the hops below suspend: a session released meanwhile keeps its
    // snapshot (it is still the answer for this call), and the map is never
    // iterated across a suspension point.
    std::vector<SessionPtr> selected;
    if (ids.empty()) {
        selected.reserve(_sessions.size());
        for (const auto& [id, session] : _sessions) {
            selected.push_back(session);
        }
    } else {
        selected.reserve(ids.size());
        for (const SessionId& id : ids) {
            // An id naming no session is skipped rather than reported: the
            // list that comes back IS the answer to "which of these exist".
            if (SessionPtr session = find_on_strand(id)) {
                selected.push_back(std::move(session));
            }
        }
    }

    // Sorted by id so a listing reads stably across turns instead of in hash
    // order. Lexicographic on "proc_<n>" is not numeric order (proc_10 before
    // proc_2), which is fine: stability is what a reader needs here, and the
    // ids are names rather than magnitudes.
    std::sort(selected.begin(), selected.end(),
              [](const SessionPtr& left, const SessionPtr& right) {
                  return left->id < right->id;
              });

    std::vector<SessionSnapshot> results;
    results.reserve(selected.size());
    for (const SessionPtr& session : selected) {
        results.push_back(co_await snapshot_on_session(session));
    }
    co_return results;
}

boost::asio::awaitable<std::optional<SessionSnapshot>>
ProcessSessionStore::snapshot(SessionId id) const
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    SessionPtr session = find_on_strand(id);
    if (session == nullptr) {
        co_return std::nullopt;
    }
    co_return co_await snapshot_on_session(std::move(session));
}

boost::asio::awaitable<std::size_t> ProcessSessionStore::size() const
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    co_return _sessions.size();
}

// ---- output -----------------------------------------------------------------

boost::asio::awaitable<std::optional<OutputRead>>
ProcessSessionStore::read_output(SessionId id, OutputStream stream, bool full)
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    SessionPtr session = find_on_strand(id);
    if (session == nullptr) {
        co_return std::nullopt;
    }

    // The cursors and the captured buffers are both session-strand state, so
    // reading and advancing happen in one pass there — no window in which a
    // second reader could see the same bytes.
    co_await boost::asio::dispatch(session->strand, boost::asio::use_awaitable);

    const process::ProcessHandle& handle = *session->handle;
    const bool want_stdout =
        stream == OutputStream::Stdout || stream == OutputStream::Both;
    const bool want_stderr =
        stream == OutputStream::Stderr || stream == OutputStream::Both;

    // One rule for both streams: full reads the whole buffer and leaves the
    // cursor where it was (an observation cannot consume another reader's
    // unread bytes), delta reads from the cursor and advances it.
    auto take = [full](const std::string& captured, bool truncated,
                       std::size_t& cursor) -> OutputSlice {
        if (full) {
            return OutputSlice{.text = captured, .truncated = truncated};
        }
        // The cursor can only exceed the buffer if the buffer shrank, which
        // it never does — the handle only appends. Clamped anyway: a cursor
        // past the end would otherwise be an out-of-range substr.
        const std::size_t from = std::min(cursor, captured.size());
        OutputSlice slice{
            .text = captured.substr(from),
            .truncated = truncated,
        };
        cursor = captured.size();
        return slice;
    };

    OutputRead read;
    if (want_stdout) {
        read.standard_output = take(handle.standard_output(),
                                    handle.stdout_truncated(),
                                    session->stdout_cursor);
    }
    if (want_stderr) {
        read.standard_error = take(handle.standard_error(),
                                   handle.stderr_truncated(),
                                   session->stderr_cursor);
    }
    // The cursors travel back whether or not this read moved them, so a
    // caller can report where a stream stands without asking again.
    read.stdout_cursor = session->stdout_cursor;
    read.stderr_cursor = session->stderr_cursor;
    co_return read;
}

// ---- input ------------------------------------------------------------------

boost::asio::awaitable<bool> ProcessSessionStore::write_input(
    SessionId id, std::string input, bool close_input)
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    SessionPtr session = find_on_strand(id);
    if (session == nullptr) {
        co_return false;
    }

    // write_input()/close_input() are the handle's two thread-safe entry
    // points (its concurrent_channel frontends), so no session-strand hop is
    // needed — and none is wanted: hopping would only add latency to a
    // fire-and-forget send.
    if (!input.empty()) {
        session->handle->write_input(std::move(input));
    }
    if (close_input) {
        // Queued messages still drain before the child sees EOF (asio
        // channels drain on close), so ordering with the write above holds.
        session->handle->close_input();
    }
    co_return true;
}

// ---- waiting ----------------------------------------------------------------

boost::asio::awaitable<std::optional<SessionSnapshot>>
ProcessSessionStore::wait_for_exit(
    SessionId id, std::uint64_t timeout_milliseconds)
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    SessionPtr session = find_on_strand(id);
    if (session == nullptr) {
        co_return std::nullopt;
    }

    // Watched, not re-waited. The handle's own await task is the one
    // authority on the terminal state (and its initial wait is one-shot by
    // contract), so this polls the observation it publishes. The cadence
    // matches that task's own probe interval — watching faster would spin
    // without noticing anything sooner.
    co_await boost::asio::dispatch(session->strand, boost::asio::use_awaitable);

    boost::asio::steady_timer poll{session->strand};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds{timeout_milliseconds};
    // BOTH conditions, and the second is not redundant: exited() is written by
    // the await task the moment it observes the child, while the output the
    // child already wrote may still be sitting in the pipes waiting for the
    // read tasks. Waiting on exited() alone reports an exit code with EMPTY
    // output for a child that printed and exited promptly — which is exactly
    // what a caller waiting for a command to finish must not be told. The pair
    // is the handle's own "work runs out" condition (process_handle.hpp).
    while (!session->handle->exited() || !session->handle->output_drained()) {
        // 0 disables the deadline: wait for the exit, however long it takes.
        if (timeout_milliseconds != 0 &&
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        poll.expires_after(kExitPollInterval);
        co_await poll.async_wait(boost::asio::use_awaitable);
    }

    // Either way the snapshot is the answer, and it carries BOTH facts: which
    // of the two conditions the wait reached is exactly what tells a caller
    // "it finished" from "it finished but its output is not all here yet" from
    // "the deadline came first". Already on the session's strand, so this
    // reads directly rather than hopping again.
    co_return SessionSnapshot{
        .id = session->id,
        .result = session->handle->snapshot(),
        .exited = session->handle->exited(),
        .output_drained = session->handle->output_drained(),
    };
}

// ---- termination and release ------------------------------------------------

boost::asio::awaitable<bool> ProcessSessionStore::terminate(
    SessionId id, bool graceful)
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    SessionPtr session = find_on_strand(id);
    if (session == nullptr) {
        co_return false;
    }

    // The handle marshals onto its own strand, so this is safe from here.
    // What comes back is "a signal was sent" — a child already gone is false
    // rather than an error, and the terminal state is still the await task's
    // to record.
    if (graceful) {
        co_return co_await session->handle->request_exit();
    }
    co_return co_await session->handle->terminate();
}

boost::asio::awaitable<std::size_t> ProcessSessionStore::terminate_all(
    bool graceful)
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);

    // The handles are collected on the store's strand BEFORE any signalling,
    // so the table is not iterated across the suspensions below: a session
    // released while this runs keeps its signal (it was live when the sweep
    // started), and no iterator outlives a hop.
    std::vector<HandlePtr> handles;
    handles.reserve(_sessions.size());
    for (const auto& [id, session] : _sessions) {
        if (session->handle) {
            handles.push_back(session->handle);
        }
    }

    std::size_t signalled = 0;
    for (const HandlePtr& handle : handles) {
        // Each handle marshals onto its own strand; an already-exited child
        // answers false, which is not counted and not an error.
        const bool sent = graceful ? co_await handle->request_exit()
                                   : co_await handle->terminate();
        if (sent) ++signalled;
    }
    // Deliberately does NOT wait for the children to die, nor release their
    // sessions: the terminal states are still the await tasks' to observe,
    // and a caller that must see them gone waits per session. Sessions stay
    // in the table so their output is still readable afterwards.
    co_return signalled;
}

boost::asio::awaitable<bool> ProcessSessionStore::release(SessionId id)
{
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    SessionPtr session = find_on_strand(id);
    if (session == nullptr) {
        co_return false;
    }

    // Refused while the child lives: dropping the table's reference could
    // drop the LAST one, and the handle's destructor kills a running child —
    // a silent kill out of what reads like a bookkeeping call. A caller that
    // means to end a child calls terminate() and says so.
    //
    // The answer is read on the SESSION's strand, where the handle keeps it,
    // and the coroutine comes back to the store's strand to act on it. The
    // detour is not ceremony: "exited() only ever transitions once" is an
    // argument about the value, not about the memory, and a release deciding a
    // lifetime question from a state read on the wrong strand is exactly the
    // kind of unsynchronised access that passes every single-runner test and
    // fails the first time the context gets a second worker thread.
    //
    // What is held across the two hops is a shared_ptr, so the session itself
    // cannot be destroyed under this coroutine; what is NOT held is any belief
    // about the table (see the re-lookup below).
    co_await boost::asio::dispatch(session->strand, boost::asio::use_awaitable);
    const bool exited = session->handle->exited();
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);

    if (!exited) {
        co_return false;
    }

    // Back on the store's strand, and the table may have moved while we were
    // away: a concurrent release() of the same session can have won the race,
    // and erasing on a stale find would drop an entry that is already gone and
    // report true a second time for one session. So the lookup is redone and
    // the identity is checked rather than the id — an id is never reused
    // (mint_id_on_strand), so "the id resolves to MY session" is the precise
    // question, and a mismatch can only mean somebody else already released
    // it.
    if (find_on_strand(id) != session) {
        co_return false;
    }

    _sessions.erase(id);
    logging::Logger::debug(std::format("process session {} released", id));
    co_return true;
}

} // namespace tools::intrinsic
