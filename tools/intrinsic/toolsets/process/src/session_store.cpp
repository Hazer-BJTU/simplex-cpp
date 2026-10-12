#include "text_output.hpp"
#include "tools/intrinsic/process/session_store.hpp"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "logging/logger.hpp"

namespace tools::intrinsic {
namespace {

// Poll session observations every 50 ms. Child exit observation has its own
// 100 ms defensive probe; this polling does not replace or accelerate that probe.
constexpr auto kExitPollInterval = std::chrono::milliseconds{50};

} // namespace

ProcessSessionStore::ProcessSessionStore(
    boost::asio::any_io_executor executor, std::size_t max_sessions)
    : _executor(std::move(executor)),
      _max_sessions(max_sessions),
      _incarnation(boost::uuids::to_string(boost::uuids::random_generator()()))
{
}

ProcessSessionStore::~ProcessSessionStore()
{
    // Callers must quiesce the context and join workers before this fallback.
    // The owned handle, rather than a copied PID, coordinates cleanup/reaping.
    // Normal shutdown still awaits shutdown() while the executor is running.
    for (const auto& [id, session] : _sessions) {
        if (session->handle) {
            session->handle->emergency_cleanup();
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

ProcessSessionStore::SessionPtr ProcessSessionStore::find_session(const SessionId& id) const
{
    const std::lock_guard lock{_sessions_mutex};
    const auto found = _sessions.find(id);
    if (found == _sessions.end()) {
        return nullptr;
    }
    return found->second;
}

void ProcessSessionStore::publish_session(SessionPtr session)
{
    const std::lock_guard lock{_sessions_mutex};
    _sessions.emplace(session->id, session);
    --_pending_spawns;
}

// ---- spawn ------------------------------------------------------------------

boost::asio::awaitable<SpawnResult> ProcessSessionStore::spawn(process::LaunchSpec spec)
{
    SessionId id;
    {
        const std::lock_guard lock{_sessions_mutex};
        const std::size_t occupied = _sessions.size() + _pending_spawns;
        if (occupied >= _max_sessions) {
            throw std::runtime_error(std::format(
                "the process session store is full: {} of {} slots are in use. "
                "Release an exited session before spawning another",
                occupied,
                _max_sessions));
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
    // The store retains children that outlive this window. Force detach so an
    // initial wait never becomes an implicit kill; callers signal explicitly.
    spec.detach_on_timeout = true;
    const std::uint64_t window = spec.initial_wait_timeout_milliseconds;

    // One strand per child, derived from the store's executor: two children
    // have no reason to queue behind each other (header, threading).
    auto session_strand = boost::asio::make_strand(_executor);

    HandlePtr handle;
    SessionPtr session;
    std::exception_ptr failure;
    try {
        handle = std::make_shared<process::ProcessHandle>(std::move(spec), session_strand);
        {
            const std::lock_guard lock{_sessions_mutex};
            id = mint_id_locked();
        }
        session = std::make_shared<Session>(Session{
            .id = id,
            .handle = handle,
            .strand = session_strand,
            .stdout_cursor = 0,
            .stderr_cursor = 0,
        });
        co_await handle->start_background_io_tasks();
        // Publish only after both I/O and the watcher are supervised. Allocation
        // failure here, or cancellation during the initial wait, rolls back the
        // complete child lifetime before returning the reserved capacity.
        publish_session(session);
        release_reservation.set_active(false);
        if (window == kNoInitialWait) {
            // Startup already owns the watcher; no detached initial-wait task.
            co_return SpawnResult{.id = id, .finished = false, .output_drained = false};
        }
        co_return co_await boost::asio::co_spawn(
            session_strand,
            [session, window]() -> boost::asio::awaitable<SpawnResult> {
                const auto& handle = session->handle;
                const bool finished = co_await handle->await_initial_execution();
                if (finished) {
                    boost::asio::steady_timer drain(session->strand);
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(window);
                    while (!handle->output_drained()
                           && std::chrono::steady_clock::now() < deadline) {
                        drain.expires_after(kExitPollInterval);
                        co_await drain.async_wait(boost::asio::use_awaitable);
                    }
                }
                co_return SpawnResult{
                    .id = session->id,
                    .finished = finished,
                    .output_drained = finished && handle->output_drained()};
            },
            boost::asio::use_awaitable);
    } catch (...) {
        failure = std::current_exception();
    }
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    if (handle) {
        try {
            co_await handle->shutdown();
        } catch (...) {
            // Preserve the initiating error. Shutdown has already attempted all
            // cleanup and joined every owned task before reporting its failure.
        }
    }
    {
        const std::lock_guard lock{_sessions_mutex};
        const auto found = _sessions.find(id);
        if (found != _sessions.end() && found->second == session) {
            _sessions.erase(found);
        }
    }
    std::rethrow_exception(failure);
}

// ---- observation ------------------------------------------------------------

boost::asio::awaitable<SessionSnapshot> ProcessSessionStore::snapshot_on_session(SessionPtr session)
{
    co_return co_await boost::asio::co_spawn(
        session->strand,
        [session]() -> boost::asio::awaitable<SessionSnapshot> {
            const auto io = session->handle->io_snapshot();
            std::string io_error;
            if (io.failure) {
                try {
                    std::rethrow_exception(io.failure);
                } catch (const std::exception& error) {
                    io_error = error.what();
                } catch (...) {
                    io_error = "Unknown process I/O failure";
                }
            }
            co_return SessionSnapshot{
                .id = session->id,
                .result = session->handle->snapshot(),
                .exited = session->handle->exited(),
                .output_drained = session->handle->output_drained(),
                .input_phase = io.input_phase,
                .input_end_requested = io.input_end_requested,
                .io_error = std::move(io_error),
            };
        },
        boost::asio::use_awaitable);
}

std::vector<ProcessSessionStore::SessionPtr>
ProcessSessionStore::select_sessions(const std::vector<SessionId>& ids) const
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
    std::sort(
        selected.begin(), selected.end(), [](const SessionPtr& left, const SessionPtr& right) {
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
ProcessSessionStore::read_output(SessionId id, OutputStream stream, bool full, bool text)
{
    auto session = find_session(id);
    if (!session) {
        co_return std::nullopt;
    }
    co_return co_await read_output_on_session(session, stream, full, text);
}

boost::asio::awaitable<OutputRead> ProcessSessionStore::read_output_on_session(
    SessionPtr session, OutputStream stream, bool full, bool text)
{
    co_return co_await boost::asio::co_spawn(
        session->strand,
        [session, stream, full, text]() -> boost::asio::awaitable<OutputRead> {
            const auto& capture = session->handle->capture();
            auto take = [&](process_io::Stream name, std::size_t& cursor) -> OutputSlice {
                const auto view = capture.view(name);
                if (!view) {
                    return {};
                }
                const auto from = full ? 0 : std::min(cursor, view->bytes.size());
                const auto raw = view->bytes.substr(from);
                OutputSlice slice;
                slice.truncated = view->truncated;
                std::size_t consumed = raw.size();
                if (text) {
                    // A still-open stream may end inside a UTF-8 sequence. Keep
                    // that raw suffix pending until it grows or capture ends.
                    auto projected = detail::text_prefix(
                        raw, view->truncated || view->end != process_io::StreamEnd::Open);
                    slice.text = std::move(projected.text);
                    consumed = projected.consumed;
                } else {
                    slice.text = raw;
                }
                // Projection and cursor advancement have no suspension between
                // them, so concurrent delta readers cannot consume the same bytes.
                if (!full) {
                    cursor = from + consumed;
                }
                return slice;
            };
            OutputRead read;
            if (stream == OutputStream::Stdout || stream == OutputStream::Both) {
                read.standard_output = take(process_io::Stream::Stdout, session->stdout_cursor);
            }
            if (stream == OutputStream::Stderr || stream == OutputStream::Both) {
                read.standard_error = take(process_io::Stream::Stderr, session->stderr_cursor);
            }
            if ((stream == OutputStream::Terminal || stream == OutputStream::Both)
                && capture.view(process_io::Stream::Terminal)) {
                read.terminal_output = take(process_io::Stream::Terminal, session->terminal_cursor);
            }
            read.stdout_cursor = session->stdout_cursor;
            read.stderr_cursor = session->stderr_cursor;
            read.terminal_cursor = session->terminal_cursor;
            co_return read;
        },
        boost::asio::use_awaitable);
}

boost::asio::awaitable<std::optional<ProcessSessionStore::SessionRead>>
ProcessSessionStore::read_session(SessionId id, bool full, bool include_output, bool release)
{
    auto session = find_session(id);
    if (!session) {
        co_return std::nullopt;
    }

    // Decide from observed exit, not merely the caller's release request.
    // Keep ownership across shutdown and projection, even if another caller
    // concurrently removes the table entry. Never hold the table mutex here.
    const bool finish = release && session->handle->exited();
    if (finish) {
        co_await session->handle->shutdown();
    }
    SessionRead result;
    if (include_output) {
        result.output = co_await read_output_on_session(session, OutputStream::Both, full, true);
    }
    result.snapshot = co_await snapshot_on_session(session);
    if (finish) {
        result.released = remove_session(session);
    }
    co_return result;
}

// ---- input ------------------------------------------------------------------

boost::asio::awaitable<bool>
ProcessSessionStore::write_input(SessionId id, std::string input, bool close_input)
{
    co_return (co_await send_input(std::move(id), std::move(input), close_input)).has_value();
}

boost::asio::awaitable<std::optional<ProcessSessionStore::InputWriteResult>>
ProcessSessionStore::send_input(SessionId id, std::string input, bool end_input)
{
    auto session = find_session(id);
    if (!session) {
        co_return std::nullopt;
    }
    co_return co_await boost::asio::co_spawn(
        session->strand,
        [session,
         input = std::move(input),
         end_input]() mutable -> boost::asio::awaitable<InputWriteResult> {
            InputWriteResult result;
            if (!input.empty()) {
                result.input = co_await session->handle->write_input(std::move(input));
            }
            if (end_input) {
                result.end = co_await session->handle->close_input();
            }
            co_return result;
        },
        boost::asio::use_awaitable);
}

// ---- waiting ----------------------------------------------------------------

boost::asio::awaitable<WaitOutcome>
ProcessSessionStore::wait_for_any(std::vector<SessionId> ids, std::uint64_t timeout_milliseconds)
{
    // The selection is taken once, on the way in: the ids are resolved to
    // handles here and the loop below only ever reads those, so a session
    // spawned while this waits is not silently added to what it waits for —
    // "the sessions named (or existing) when the call arrived" is a set the
    // caller can reason about, and one that grew mid-wait is not.
    const std::vector<SessionPtr> selected = select_sessions(ids);

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds{timeout_milliseconds};
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
        finished =
            std::any_of(snapshots.begin(), snapshots.end(), [](const SessionSnapshot& snapshot) {
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
        if (selected.empty() || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        poll.expires_after(kExitPollInterval);
        co_await poll.async_wait(boost::asio::use_awaitable);
    }

    WaitOutcome outcome;
    outcome.snapshots = std::move(snapshots);
    outcome.finished = finished;
    outcome.timed_out = !finished;
    outcome.waited_milliseconds =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count());
    co_return outcome;
}

// ---- termination and release ------------------------------------------------

boost::asio::awaitable<bool> ProcessSessionStore::terminate(SessionId id, bool graceful)
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

boost::asio::awaitable<std::size_t> ProcessSessionStore::terminate_all(bool graceful)
{
    // Copy ownership under the table mutex, then signal without holding it.
    const auto selected = select_sessions({});

    std::size_t signalled = 0;
    for (const SessionPtr& session : selected) {
        const HandlePtr& handle = session->handle;
        // Each handle marshals onto its own strand; an already-exited child
        // answers false, which is not counted and not an error.
        const bool sent = graceful ? co_await handle->request_exit() : co_await handle->terminate();
        if (sent) {
            ++signalled;
        }
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
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    std::exception_ptr failure;
    for (const auto& session : selected) {
        try {
            co_await session->handle->shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    {
        const std::lock_guard lock{_sessions_mutex};
        _sessions.clear();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
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
    co_return remove_session(session);
}

bool ProcessSessionStore::remove_session(const SessionPtr& session)
{
    const auto& id = session->id;
    SessionPtr removed;
    {
        const std::lock_guard lock{_sessions_mutex};
        const auto found = _sessions.find(id);
        if (found == _sessions.end() || found->second != session) {
            return false;
        }
        // exited() is an atomic, monotonic latch. Lookup, check and removal
        // form one transaction, so exactly one concurrent release succeeds.
        removed = std::move(found->second);
        _sessions.erase(found);
    }
    // Retain ownership until after unlocking: destruction and logging must
    // not extend the table's critical section.
    logging::Logger::debug(std::format("process session {} released", id));
    return true;
}

} // namespace tools::intrinsic
