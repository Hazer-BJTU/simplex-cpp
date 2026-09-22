#pragma once

//
// process/session_store.hpp — many children, addressed by name
// =============================================================
//
// ProcessHandle is ONE child per instance, held by shared_ptr, driven by
// whoever owns it. A model needs something else: a stable name it can come
// back to across turns ("proc_1"), several children at once, and "what is new
// since I last looked". This store is that layer — the agent-facing
// bookkeeping process/ deliberately does not do, because process/ does not
// know what a child is FOR.
//
// Three things live here and nowhere else:
//
//   SESSION IDENTITY   a SessionId per child, minted "proc_<n>". Text, not an
//                      integer, because it travels through JSON tool
//                      arguments and back as prose the model may repeat. The
//                      numbers only ever go UP: an id is handed out once and
//                      never reused, not even after its session is released.
//                      Reuse would alias a stale handle in the one place
//                      aliasing is unrecoverable — a model's context still
//                      says "proc_1 is the build I started three turns ago",
//                      and a reused proc_1 would make send_process end a
//                      process that model never saw. Growth is the cheap
//                      side of that trade: a uint64 counter does not run out.
//   READ CURSORS       how much of each stream the model has already been
//                      given. ProcessHandle keeps the whole captured buffer
//                      and never forgets, so "the new output" is a cursor
//                      over that buffer — this layer's account, not the
//                      child's state.
//   REAPING            a session is removed only once its child is observed
//                      terminal, and its id goes nowhere afterwards: the
//                      table shrinks, the number does not come back.
//
// THREADING — TWO LEVELS OF STRAND, on purpose:
//
//   the STORE's strand      serialises the table itself (the map, the id
//                           pool). Every public coroutine begins with
//                           `co_await dispatch(_strand, ...)`, so callers may
//                           come from any executor without locking — the
//                           pattern ProcessHandle itself follows.
//   a SESSION's strand      one per child, derived from the store's executor.
//                           Every ProcessHandle member except its stdin
//                           channel belongs to its own strand, and two
//                           children have no reason to queue behind each
//                           other: a session's handle, and its cursors, are
//                           touched only there.
//
// The store's methods therefore hop: onto the store's strand to find the
// session, onto that session's strand to read or drive its handle. That hop
// is why they are coroutines rather than plain accessors, and why a caller
// never gets a Session out — a handle read off-strand would be a data race,
// so what leaves this class is always a VALUE (a snapshot, a slice of output,
// a bool), never a reference into the table. release() is the one method that
// hops TWICE (in, out, and back in), because it must both read the handle's
// terminal state where that state lives and remove the table entry where the
// table lives; see its own comment.
//
// THE STORE OWNS THE LIFECYCLE CONTRACT. ProcessHandle's contract
// (process_handle.hpp) says: start the io tasks, then keep driving the handle
// to a terminal observation, or it keeps itself alive forever. spawn() honours
// that in full — construct, start the io tasks, then drive the handle through
// await_initial_execution(), either inline (when an initial-wait window was
// asked for) or on a detached task (when it was not).
//
// spawn() is therefore the one call that may TAKE TIME, bounded by that window,
// and that is deliberate: it is what lets one launch serve both a command whose
// result is wanted now and a server that must simply start. A child that
// outlives its window is detached and becomes an ordinary live session, so the
// wait is a grace period and never a lifetime cap. Waiting for an exit that the
// window did not cover is wait_for_any()'s job, separately and with its own
// deadline.
//
// SHUTDOWN IS terminate_all(), NOT THE DESTRUCTOR, and the reason is worth
// knowing before owning one. Each handle's await task holds a reference to its
// own handle until the child's terminal state is observed (ProcessHandle's
// documented lifetime model), so dropping this table does NOT destroy the
// handles and does NOT stop their children — the store's last reference is
// simply not the last one. Killing a child is a coroutine, and a destructor
// has no executor left to run one on. So a host ends its children by awaiting
// terminate_all() while its context still runs; the destructor is a
// last-resort tail that signals whatever is left synchronously (::kill on the
// pid recorded at spawn) and says so loudly, because a leaked child outlives
// the process that made it.
//
// The destructor therefore reads exactly two things off a handle, and both are
// chosen because a destructor cannot hop to a strand: the pid, which the
// spawn stamped and nothing ever writes again, and exited(), which is the
// handle's one thread-safe observation — a latch, so that "this child was
// already observed (and therefore reaped)" is answerable from any thread. The
// pair is what keeps the tail honest: a child that was observed is not
// signalled, because its pid may already belong to somebody else.
//
// DROP THE TABLE AFTER THE CONTEXT IS QUIESCED — stop it and join its threads
// first, whether or not terminate_all() was awaited. Signalling a pid needs no
// executor, so the tail works either way, but the table's own strand shares
// refcounted state with the operations still queued on that context, and
// freeing it from another thread while a worker finishes one is a data race in
// that refcount (measured: ThreadSanitizer reports it in operator delete, on
// every suite that dropped the store with its workers still running). Nothing
// that shares an executor with a running context should be destroyed before
// the context is quiesced.
//

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/types.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>

#include "dataclass/process_spec.hpp"
#include "process/process_handle.hpp"

namespace tools::intrinsic {

/// Which stream a read targets. Both is the common case for a tool result —
/// a model reading a command's output usually wants stderr next to stdout.
enum class OutputStream { Stdout, Stderr, Both };

/// One stream's captured text as a reader receives it: the bytes asked for,
/// and whether the capture behind them stopped at the spec's cap.
struct OutputSlice {
    std::string text;
    bool truncated = false;
};

/// What a read hands back: the streams that were asked for, and where the
/// cursors stand afterwards. The cursors travel so a caller can report "you
/// have now seen N bytes" without a second round trip.
struct OutputRead {
    OutputSlice standard_output;
    OutputSlice standard_error;
    std::size_t stdout_cursor = 0;
    std::size_t stderr_cursor = 0;
};

/// One session as a caller sees it: its name, the report-shaped view of its
/// child, and the two facts about its ending — whether the child has been
/// observed terminal, and whether the capture behind it is COMPLETE.
///
/// Those last two are NOT the same question, and a caller that treats them as
/// one reports an exit code with half its output:
///
///   exited          the child is gone (its exit code is readable).
///   output_drained  both output pipes have hit EOF and the readers have
///                   finished, so the captured text is everything the child
///                   will ever produce.
///
/// The gap between them is ordinary, not exotic. The handle's await task
/// records the exit the moment it observes the child, while the readers are
/// still draining what sits in the pipe buffers — and a descendant that
/// inherited stdout/stderr keeps those pipes OPEN after the direct child is
/// gone, so `exited` can be true for as long as that descendant lives while
/// `output_drained` stays false. A VALUE — the live handle never leaves the
/// store (see the header's threading note).
struct SessionSnapshot {
    std::string id;
    process::ExecutionResult result;
    bool exited = false;
    bool output_drained = false;
};

/// What a spawn answers with: the session's id, whether the child finished
/// inside the spec's initial-wait window, and whether its output was fully
/// collected by the time this answered.
///
/// `finished` is the whole point of the pair with the id. A caller that
/// launched `ls` wants its output in the same call, while one that launched a
/// server wants the id and nothing else — and which of the two happened is not
/// something the caller can know in advance, only something the launch can
/// report. `finished` true means the session is already terminal (its exit
/// code readable); false means it is a live session to come back to.
///
/// `output_drained` is the SECOND question, and it is deliberately not folded
/// into the first. `finished` is a fact about the child; the capture is a fact
/// about its pipes, and the two part company exactly where it matters — a
/// child that exits while a descendant holds its inherited stdout/stderr open
/// is finished with its output still arriving. So `finished && !output_drained`
/// is a real answer ("it ended, but this result is not the whole of what it
/// printed"), and the tools report both rather than letting one imply the
/// other.
struct SpawnResult {
    std::string id;
    bool finished = false;
    bool output_drained = false;
};

/// What a wait_for_any() answered with: every selected session as it stood
/// when the wait ended, why the wait ended, and how long it took.
///
/// The two flags are a pair rather than one enum because the question a caller
/// asks of them is binary — "did something finish, or did I run out of time" —
/// and `timed_out` is defined as exactly `!finished`, so a reader can never see
/// them disagree. What they do NOT say is which session finished: that is on
/// the snapshots, per session, as `exited` and `output_drained`. There may be
/// more than one finished session in the list (the ones that were already
/// finished when the call arrived), which is why the count is taken from the
/// snapshots and not from the flags.
struct WaitOutcome {
    std::vector<SessionSnapshot> snapshots;
    bool finished = false;
    bool timed_out = false;
    std::uint64_t waited_milliseconds = 0;
};

/**
 * The table of live child processes, addressed by session id.
 *
 * Held by shared_ptr: the tools share one store, and the store's own
 * coroutines outlive any single tool call. Not copyable — a copy would be a
 * second table over the same children, each minting ids the other does not
 * know about (the same reason ToolRegistry refuses to be copied).
 */
class ProcessSessionStore
    : public std::enable_shared_from_this<ProcessSessionStore> {
public:
    using SessionId = std::string;
    using HandlePtr = std::shared_ptr<process::ProcessHandle>;

    /// How many sessions may be RETAINED at once. A model that loops on spawn
    /// would otherwise fill the host's process table; the refusal is a
    /// failure the tool reports, which the model can read and act on.
    ///
    /// Retained, not alive: this counts table entries, and an exited session
    /// stays in the table until it is released (its output is still worth
    /// reading). So a model that spawns 32 quick commands without releasing
    /// any of them is refused on the 33rd, exactly as if they were all still
    /// running, and the remedy the message names is release_exited — the
    /// answer the cap is really about.
    static constexpr std::size_t kDefaultMaxSessions = 32;

    explicit ProcessSessionStore(
        boost::asio::any_io_executor executor,
        std::size_t max_sessions = kDefaultMaxSessions);
    ~ProcessSessionStore();

    ProcessSessionStore(const ProcessSessionStore&) = delete;
    ProcessSessionStore& operator = (const ProcessSessionStore&) = delete;
    ProcessSessionStore(ProcessSessionStore&&) = delete;
    ProcessSessionStore& operator = (ProcessSessionStore&&) = delete;

    /**
     * Launch a child, register it under a fresh session id, and wait out the
     * spec's INITIAL-WAIT WINDOW before answering.
     *
     * This is the one call that is deliberately allowed to take time, and the
     * window is what makes one tool serve two very different jobs. A quick
     * command finishes inside it, so the answer already carries a terminal
     * session — the caller reads its output immediately and never has to poll.
     * A long-running one outlives it, and the answer is a live session id to
     * come back to. Which of those happened is reported, not guessed
     * (SpawnResult::finished).
     *
     * That is exactly what ProcessHandle::await_initial_execution() is for
     * (process_handle.hpp): the window is an initial-wait GRACE PERIOD, not a
     * lifetime budget, and a child that outlives it is detached and keeps
     * running under a fresh await task that records its eventual exit. So the
     * spec's `initial_wait_timeout_milliseconds` is HONOURED here rather than
     * overwritten.
     *
     * Two fields are still the store's, because the alternative is a leak
     * rather than a policy:
     *
     *   detach_on_timeout is forced ON. A session is a child a caller can come
     *   back to, so a store that killed a child at the end of the window would
     *   hand out ids for processes it had just destroyed — and `send_process`
     *   already exists for a caller that wants the child dead.
     *
     *   A window of 0 means "wait indefinitely" to the handle, which would
     *   hang a spawn on a child that never exits. Here it means the opposite —
     *   do not wait at all — because "start it and give me the id" is a
     *   legitimate request and hanging is not. kNoInitialWait spells it.
     *
     * The wait is bounded by the window, so it does not block the store's
     * strand: the table is updated and released before the wait begins.
     *
     * @return the new session's id, whether the child finished inside the
     *         window, and whether the capture was complete when this answered.
     *         The last two are separate on purpose (SpawnResult): a child that
     *         exits while a descendant keeps its pipes open is finished with
     *         output still to come, and the drain is bounded by the same window
     *         so that case answers rather than hanging.
     * @throws process::ProcessException when the launch itself fails (the
     *         spawn stages: Environment / ResolveExecutable / Spawn), passed
     *         through untranslated so the caller keeps the launch context.
     * @throws std::runtime_error when the store is already at its session
     *         cap.
     */
    boost::asio::awaitable<SpawnResult> spawn(process::LaunchSpec spec);

    /// The spec value that means "do not wait for this child at all" — the
    /// answer is the id, and the caller polls or waits later. Spelled out
    /// because the handle reads 0 as "wait indefinitely", which is the one
    /// thing a spawn must not do.
    static constexpr std::uint64_t kNoInitialWait = 0;

    /// Every session's snapshot, in id order (so a listing reads stably
    /// across turns rather than in hash order). `ids` empty = all of them;
    /// ids naming no session are skipped, since a caller listing a stale id
    /// is asking a question the snapshot list already answers.
    boost::asio::awaitable<std::vector<SessionSnapshot>> snapshots(
        std::vector<SessionId> ids = {}) const;

    /// One session's snapshot, or nullopt when no session has that id.
    boost::asio::awaitable<std::optional<SessionSnapshot>> snapshot(
        SessionId id) const;

    /**
     * Read captured output for one session.
     *
     * @param full false — the DELTA: the bytes appended since the last
     *        delta read, and the cursor advances past them. true — the whole
     *        captured buffer, and the cursor DOES NOT move: a full read is
     *        an observation, not a consumption, so it cannot cost a
     *        concurrent delta reader its unread bytes.
     * @return nullopt when no session has that id.
     */
    boost::asio::awaitable<std::optional<OutputRead>> read_output(
        SessionId id, OutputStream stream, bool full);

    /// Queue text for the child's stdin, and optionally end stdin after it.
    /// Ordering is the handle's channel's; delivery is not awaited (see
    /// ProcessHandle::write_input). False = no session with that id.
    boost::asio::awaitable<bool> write_input(
        SessionId id, std::string input, bool close_input);

    /**
     * Wait for ANY one of these sessions to FINISH, and answer with ALL of
     * them either way.
     *
     * This is the set-general form of "wait for the child", and the two
     * properties that make it one are worth stating separately, because each
     * answers a different question:
     *
     *   the WAIT ends on the first session observed finished (or on the
     *   deadline, whichever comes first) — one child ending is what a caller
     *   waiting on a fan of them is waiting for, and the call does not then
     *   wait for the rest;
     *
     *   the ANSWER is every session in the selection, not just the one that
     *   ended: the siblings are the context the ended one is read in, and
     *   reporting only the finisher would cost a second call to learn what
     *   the others did meanwhile. Nothing about them is touched — a wait
     *   changes no child's state, and the sessions that were running when the
     *   call arrived are running when it returns.
     *
     * FINISHED means the handle's own "work runs out" pair — exited AND
     * output_drained — the same condition the single-session wait used, and
     * the same reason: exited() is published the moment the child is observed,
     * while the bytes it printed may still be in the pipes, so answering on
     * exited() alone would report an exit code with half its output. A
     * selection with a session that has already finished therefore answers
     * immediately, in one pass and without waiting at all.
     *
     * POLLED, not signalled. Each pass reads the handles' observations on a
     * short cadence (kExitPollInterval); nothing here registers a callback on
     * a child or wakes a waiter, so the cost of watching is a timer that fires
     * a few times a second while the call is outstanding. That is the trade
     * this method deliberately makes: the handles' await tasks remain the one
     * authority on terminal state (their initial wait is one-shot by
     * contract), and a poll over a handful of sessions costs nothing worth
     * building a notification path for.
     *
     * @param ids the sessions to watch. EMPTY means every session in the
     *        table, exactly as snapshots() reads it; ids naming no session are
     *        skipped, since the answer's list is what says which exist. A
     *        selection that resolves to nothing returns at once rather than
     *        waiting out a deadline over no children.
     * @param timeout_milliseconds how long to watch before answering anyway.
     *        0 does not wait at all: the first pass IS the answer, which is
     *        how a caller asks "what is going on right now".
     * @return the snapshots of every selected session, in the id order
     *         snapshots() uses, plus whether the wait ended on a finished
     *         session or on the deadline, and how long it actually waited.
     *         `timed_out` is exactly the negation of `finished`: neither an
     *         empty selection nor a timeout of 0 can end on a session that
     *         never finished.
     */
    boost::asio::awaitable<WaitOutcome> wait_for_any(
        std::vector<SessionId> ids, std::uint64_t timeout_milliseconds);

    /// Signal one session's child to end: SIGKILL, or SIGTERM when
    /// @p graceful. The terminal state is still recorded by the await task,
    /// so a caller that needs it follows with wait_for_any(). False = no
    /// session with that id, or its child was already gone.
    boost::asio::awaitable<bool> terminate(SessionId id, bool graceful);

    /**
     * Drop an EXITED session. Its id is spent — nobody gets "proc_3" again.
     *
     * Refuses a session whose child is still running: releasing it would
     * drop the last reference to a live child, and the handle's destructor
     * would kill it — a silent kill from what reads like a bookkeeping call.
     * A caller that means to end a child calls terminate() and says so.
     *
     * Reads the child's terminal state on the SESSION's strand, where the
     * handle keeps it, and removes the entry on the STORE's strand, where the
     * table lives — hopping out and back rather than reading strand state from
     * the wrong side (`exited()` is a latch and would be safe either way, but
     * the rule is the rule, and this is the method that made it worth stating:
     * "the value only ever transitions once" is not a synchronisation
     * argument). Because the removal happens after a hop, the id is looked up
     * AGAIN when the coroutine gets back: a concurrent release() of the same
     * session may have won the race while this one was away, and the second
     * one has to answer false rather than erase a second time.
     *
     * @return whether the session was dropped (false: unknown id, the child
     *         is still running, or another caller released it first).
     */
    boost::asio::awaitable<bool> release(SessionId id);

    /// How many sessions the table RETAINS — exited-but-unreleased ones
    /// included, since that is what it holds and what the cap counts. For
    /// diagnostics and the cap check; a host wanting to know how many
    /// children are alive asks the snapshots.
    boost::asio::awaitable<std::size_t> size() const;

    /**
     * End every live child — the host's shutdown path, and the one to call
     * before dropping the store.
     *
     * This exists because the destructor CANNOT do it properly: each handle's
     * own await task holds a reference to its handle until the child's
     * terminal state is observed, so merely dropping the table does not
     * destroy a handle and does not stop its child. Killing needs a
     * coroutine, and a destructor has no executor to run one on.
     *
     * @param graceful SIGTERM rather than SIGKILL. A graceful sweep does not
     *        wait for the children to act on it: a caller that must see them
     *        gone follows with wait_for_any() over their ids.
     * @return how many children were signalled (an already-exited child is
     *         not one).
     */
    boost::asio::awaitable<std::size_t> terminate_all(bool graceful = false);

private:
    /// One managed child plus this layer's own bookkeeping. Held by
    /// shared_ptr so a session survives a concurrent release() while a read
    /// is still on its strand.
    struct Session {
        SessionId id;
        HandlePtr handle;
        boost::asio::strand<boost::asio::any_io_executor> strand;
        /// The pid the handle stamped, copied at spawn because the destructor
        /// signals it without a strand to read the handle on (the pid is
        /// written once, at construction, and never again).
        pid_t pid = -1;
        // Bytes of each stream already handed to a delta reader. Touched
        // only on `strand`.
        std::size_t stdout_cursor = 0;
        std::size_t stderr_cursor = 0;
    };
    using SessionPtr = std::shared_ptr<Session>;

    /// The session for `id`, or nullptr. Callers must already be on the
    /// store's strand.
    [[nodiscard]] SessionPtr find_on_strand(const SessionId& id) const;

    /// Mint an id: the next number, always. Store's strand only, though the
    /// monotonic counter alone would make it safe from anywhere — the pool
    /// that used to sit here is gone precisely because a minted id must never
    /// name two children (see the header's identity note).
    [[nodiscard]] SessionId mint_id_on_strand();

    /// The snapshot for `session`, read on its OWN strand.
    static boost::asio::awaitable<SessionSnapshot> snapshot_on_session(
        SessionPtr session);

    /// The sessions `ids` names, in the id order the snapshots come back in —
    /// shared_ptr copies, so a release() during the hops that follow cannot
    /// pull a session out from under the caller that selected it. Empty `ids`
    /// selects the whole table; unknown ids are skipped. Callers must already
    /// be on the store's strand, and must not hold the result across a
    /// suspension point that could re-enter the table (they may hold it across
    /// a session-strand hop, which is exactly what both readers do).
    [[nodiscard]] std::vector<SessionPtr> select_on_strand(
        const std::vector<SessionId>& ids) const;

    mutable boost::asio::strand<boost::asio::any_io_executor> _strand;
    boost::asio::any_io_executor _executor;
    std::size_t _max_sessions;

    // The table: store's strand only. _next_id only climbs.
    std::unordered_map<SessionId, SessionPtr> _sessions;
    std::uint64_t _next_id = 1;
};

} // namespace tools::intrinsic
