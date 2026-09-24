#include "tools/intrinsic/process/session_store.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>

#include <cerrno>
#include <csignal>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "logging/logger.hpp"

namespace tools::intrinsic {
namespace {

// How often the waiting reader re-reads the handles' observations. Matched to
// ProcessHandle's own probe cadence: the await task there notices a missed
// exit within one such interval, so watching faster would only spin.
constexpr auto kExitPollInterval = std::chrono::milliseconds{50};

} // namespace

ProcessSessionStore::ProcessSessionStore(
    boost::asio::any_io_executor executor,
    std::size_t max_sessions
): _executor(std::move(executor)),
   _max_sessions(max_sessions),
   _incarnation(boost::uuids::to_string(boost::uuids::random_generator()()))
{}

ProcessSessionStore::~ProcessSessionStore()
{
    // The last-resort tail, not the shutdown mechanism — shutdown() is
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
            "still running; killing it. A host should await shutdown() "
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

ProcessSessionStore::SessionId ProcessSessionStore::mint_id_locked()
{
    // Monotonic, always. No pool, no reuse: an agent-facing id that came back
    // around would alias a process the model remembers from an earlier turn
    // (the header's identity note). The cost of never reusing is a number
    // that grows; the cost of reuse is killing the wrong process.
    return std::format("proc_{}_{}", _incarnation, _next_id++);
}

ProcessSessionStore::SessionPtr ProcessSessionStore::find_session(
    const SessionId& id) const
{
    const std::lock_guard lock{_sessions_mutex};
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
    SessionId id;
    {
        const std::lock_guard lock{_sessions_mutex};
        const std::size_t occupied = _sessions.size() + _pending_spawns;
        if (occupied >= _max_sessions) {
            throw std::runtime_error(std::format(
                "the process session store is full: {} of {} slots are in use. "
                "Release an exited session before spawning another",
                occupied, _max_sessions));
        }
        ++_pending_spawns;
    }
    // Keep this slot reserved across launch and I/O startup. No mutex is
    // held while constructing a process or awaiting its strand operation.
    boost::scope::scope_exit release_reservation{[this] {
        const std::lock_guard lock{_sessions_mutex};
        --_pending_spawns;
    }};

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
    {
        const std::lock_guard lock{_sessions_mutex};
        id = mint_id_locked();
    }

    auto session = std::make_shared<Session>(Session{
        .id = id,
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

    // Exception-safety limit: if publication throws (for example, bad_alloc),
    // the capacity reservation is returned, but I/O tasks have already started
    // and the terminal watcher has not. Full rollback of this partial lifecycle
    // requires a separate ProcessHandle lifecycle/RAII change.
    // Publish before the initial wait so concurrent observations can find
    // the running child. Converting the reservation into an entry is atomic.
    {
        const std::lock_guard lock{_sessions_mutex};
        _sessions.emplace(id, session);
        --_pending_spawns;
        release_reservation.set_active(false);
    }
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
                // With the deadline disabled, wait for terminal observation
                // or watcher shutdown. Any watcher failure is retained by
                // the handle for the explicit shutdown lifetime fence.
                co_await handle->await_initial_execution();
            },
            boost::asio::detached);
        co_return SpawnResult{.id = id, .finished = false, .output_drained = false};
    }

    // Both the initial wait and every output-drain check execute on the
    // session strand, even after a timer or handle operation suspends.
    co_return co_await boost::asio::co_spawn(
        session_strand,
        [session, window]() -> boost::asio::awaitable<SpawnResult> {
            const auto& handle = session->handle;
            const bool finished = co_await handle->await_initial_execution();
            if (finished) {
                boost::asio::steady_timer drain{session->strand};
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds{window};
                while (!handle->output_drained() &&
                       std::chrono::steady_clock::now() < deadline) {
                    drain.expires_after(kExitPollInterval);
                    co_await drain.async_wait(boost::asio::use_awaitable);
                }
            }
            // A descendant may keep the output pipes open after child exit.
            co_return SpawnResult{
                .id = session->id,
                .finished = finished,
                .output_drained = finished && handle->output_drained(),
            };
        },
        boost::asio::use_awaitable
    );
}

// ---- observation ------------------------------------------------------------

boost::asio::awaitable<SessionSnapshot>
ProcessSessionStore::snapshot_on_session(SessionPtr session)
{
    co_return co_await boost::asio::co_spawn(
        session->strand,
        [session]() -> boost::asio::awaitable<SessionSnapshot> {
            co_return SessionSnapshot{
                .id = session->id,
                .result = session->handle->snapshot(),
                .exited = session->handle->exited(),
                .output_drained = session->handle->output_drained(),
            };
        },
        boost::asio::use_awaitable
    );
}

std::vector<ProcessSessionStore::SessionPtr> ProcessSessionStore::select_sessions(
    const std::vector<SessionId>& ids) const
{
    std::vector<SessionPtr> selected;
    {
        const std::lock_guard lock{_sessions_mutex};
        if (ids.empty()) {
            selected.reserve(_sessions.size());
            for (const auto& [id, session] : _sessions) {
                selected.push_back(session);
            }
        } else {
            selected.reserve(ids.size());
            for (const SessionId& id : ids) {
                const auto found = _sessions.find(id);
                if (found != _sessions.end()) {
                    selected.push_back(found->second);
                }
            }
        }
    }

    // Sorted by id so a listing reads stably across turns instead of in hash
    // order. Lexicographic on "proc_<uuid>_<n>" is not counter order,
    // which is fine: stability is what a reader needs here, and the
    // ids are names rather than magnitudes.
    std::sort(selected.begin(), selected.end(),
              [](const SessionPtr& left, const SessionPtr& right) {
                  return left->id < right->id;
              });
    return selected;
}

boost::asio::awaitable<std::vector<SessionSnapshot>>
ProcessSessionStore::snapshots(std::vector<SessionId> ids) const
{
    const std::vector<SessionPtr> selected = select_sessions(ids);
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
    SessionPtr session = find_session(id);
    if (session == nullptr) {
        co_return std::nullopt;
    }
    co_return co_await snapshot_on_session(std::move(session));
}

boost::asio::awaitable<std::size_t> ProcessSessionStore::size() const
{
    const std::lock_guard lock{_sessions_mutex};
    co_return _sessions.size();
}

// ---- output -----------------------------------------------------------------

boost::asio::awaitable<std::optional<OutputRead>>
ProcessSessionStore::read_output(SessionId id, OutputStream stream, bool full)
{
    SessionPtr session = find_session(id);
    if (session == nullptr) {
        co_return std::nullopt;
    }

    // Copy output and advance cursors in one strand operation. No suspension
    // occurs inside this transaction, so delta readers cannot consume twice.
    co_return co_await boost::asio::co_spawn(
        session->strand,
        [session, stream, full]() -> boost::asio::awaitable<OutputRead> {
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
        },
        boost::asio::use_awaitable
    );
}

// ---- input ------------------------------------------------------------------

boost::asio::awaitable<bool> ProcessSessionStore::write_input(
    SessionId id, std::string input, bool close_input)
{
    SessionPtr session = find_session(id);
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

boost::asio::awaitable<WaitOutcome>
ProcessSessionStore::wait_for_any(
    std::vector<SessionId> ids, std::uint64_t timeout_milliseconds)
{
    // The selection is taken once, on the way in: the ids are resolved to
    // handles here and the loop below only ever reads those, so a session
    // spawned while this waits is not silently added to what it waits for —
    // "the sessions named (or existing) when the call arrived" is a set the
    // caller can reason about, and one that grew mid-wait is not.
    const std::vector<SessionPtr> selected = select_sessions(ids);

    const auto started = std::chrono::steady_clock::now();
    const auto deadline =
        started + std::chrono::milliseconds{timeout_milliseconds};
    // The timer belongs to this call; only handle observations need a strand.
    boost::asio::steady_timer poll{co_await boost::asio::this_coro::executor};

    std::vector<SessionSnapshot> snapshots;
    bool finished = false;
    for (;;) {
        // Every pass reads EVERY selected session, even once one of them is
        // known finished: the answer is the whole set, and a pass that stopped
        // early would report one session's ending beside stale neighbours.
        snapshots.clear();
        snapshots.reserve(selected.size());
        for (const SessionPtr& session : selected) {
            snapshots.push_back(co_await snapshot_on_session(session));
        }
        // The handle's own "work runs out" pair, and the second half is not
        // redundant: exited() is written by the await task the moment it
        // observes the child, while the bytes the child already wrote may
        // still be sitting in the pipes waiting for the read tasks. Ending the
        // wait on exited() alone answers with an exit code and EMPTY output
        // for a command that printed and exited promptly — the one thing a
        // caller waiting for a command must not be told
        // (ProcessHandle::output_drained).
        finished = std::any_of(
            snapshots.begin(), snapshots.end(),
            [](const SessionSnapshot& snapshot) {
                return snapshot.exited && snapshot.output_drained;
            });
        if (finished) {
            break;
        }
        // The deadline, and 0 falls out of it rather than being a special
        // case: 0 puts the deadline in the past, so the pass just taken is the
        // answer. A selection that resolved to nothing breaks here too — there
        // is no child whose ending could end the wait, and holding the call
        // open over an empty table would spend a timeout to learn nothing.
        if (selected.empty() ||
            std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        poll.expires_after(kExitPollInterval);
        co_await poll.async_wait(boost::asio::use_awaitable);
    }

    WaitOutcome outcome;
    outcome.snapshots = std::move(snapshots);
    outcome.finished = finished;
    outcome.timed_out = !finished;
    outcome.waited_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    co_return outcome;
}

// ---- termination and release ------------------------------------------------

boost::asio::awaitable<bool> ProcessSessionStore::terminate(
    SessionId id, bool graceful)
{
    SessionPtr session = find_session(id);
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
    // Copy ownership under the table mutex, then signal without holding it.
    const auto selected = select_sessions({});

    std::size_t signalled = 0;
    for (const SessionPtr& session : selected) {
        const HandlePtr& handle = session->handle;
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

boost::asio::awaitable<void> ProcessSessionStore::shutdown()
{
    const auto selected = select_sessions({});
    co_await boost::asio::this_coro::reset_cancellation_state(
        boost::asio::disable_cancellation());
    std::exception_ptr failure;
    for (const auto& session : selected) {
        try {
            co_await session->handle->shutdown();
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    }
    {
        const std::lock_guard lock{_sessions_mutex};
        _sessions.clear();
    }
    if (failure) std::rethrow_exception(failure);
}

boost::asio::awaitable<bool> ProcessSessionStore::release(SessionId id)
{
    // A released handle must not outlive the store through self-owning pipe
    // tasks. Capture ownership before suspending; never hold the table mutex.
    auto session = find_session(id);
    if (!session || !session->handle->exited()) {
        co_return false;
    }
    co_await session->handle->shutdown();
    SessionPtr removed;
    {
        const std::lock_guard lock{_sessions_mutex};
        const auto found = _sessions.find(id);
        if (found == _sessions.end() || !found->second->handle->exited()) {
            co_return false;
        }
        // exited() is an atomic, monotonic latch. Lookup, check and removal
        // form one transaction, so exactly one concurrent release succeeds.
        removed = std::move(found->second);
        _sessions.erase(found);
    }
    // Retain ownership until after unlocking: destruction and logging must
    // not extend the table's critical section.
    logging::Logger::debug(std::format("process session {} released", id));
    co_return true;
}

} // namespace tools::intrinsic
