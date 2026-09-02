#pragma once

//
// process_handle.hpp — one managed child, from spawn to final status
// ===================================================================
//
// ProcessHandle is the process/ module's manager core: a single child
// process together with its three pipes, the coroutines that feed and drain
// them, and the deadline policy from its LaunchSpec. It knows nothing about
// what the child is FOR — callers speak the typed data contract
// (dataclass/process_spec.hpp) in, ExecutionResult snapshots out.
//
// Construction IS the spawn. The constructor assembles the child's
// environment (see below), resolves the executable on PATH, wires the
// pipes, launches, and stamps pid + started_at. A failure anywhere in
// there throws process::ProcessException (stages Environment /
// ResolveExecutable / Spawn) and leaves no child and no tasks behind, so a
// handle that exists is a handle whose child is running.
//
// Environment assembly is a MERGE, not a concatenation: execve entries
// with duplicated names are undefined (glibc's getenv returns the FIRST
// match), so inherited entries and the spec's KEY=VALUE entries are merged
// by key — an explicit entry replaces the inherited entry of the same key
// (later explicit entries beat earlier ones), and the child's environ ends
// up with unique keys only. Each explicit entry must be a well-formed
// KEY=VALUE string (an '=' with a non-empty key); anything else throws at
// Stage::Environment instead of launching a child with a broken environ.
// The SAME assembled environment drives executable resolution: a PATH
// supplied by the spec is honored (inherit_environment=false plus
// PATH=<custom dirs> resolves "tool" against <custom dirs>). When the
// assembled environment has no PATH at all, resolution falls back to the
// PARENT's PATH — a documented convenience so bare names still resolve;
// the child's own environment is unaffected by the fallback and really
// does ship without a PATH.
//
// Lifetime model — read this before owning one:
//   - The class MUST live in a std::shared_ptr: the background tasks
//     capture shared_from_this(), so once start_background_io_tasks() has
//     run, the handle keeps ITSELF alive until its work runs out.
//   - Work runs out when the child's terminal state has been observed
//     (the await task then closes the stdin channel — a dead child cannot
//     read) and both output pipes have hit EOF. At that point the last
//     self-reference drops and an owner releasing its shared_ptr destroys
//     the handle normally. close_input() is the manual form for reaching
//     stdin-EOF early (interactive children waiting for input).
//   - The destructor is therefore a defensive tail, not the teardown
//     mechanism: it closes whatever is still open, and force-terminates a
//     child that is somehow still alive. Detach is a within-lifetime
//     concept — the deadline policy leaving a child running while the
//     handle keeps observing it; destroying a handle whose child lives is
//     an abandonment this class resolves by killing (SIGKILL, reaped),
//     never by leaking the child. A child that exited unobserved is reaped
//     on the way out (running() waitpid's).
//
// Strand discipline: every member except _write_channel belongs to the
// handle's strand. Start the two lifecycle coroutines ON that strand (the
// tests' make_strand + co_spawn pattern), and treat the observation
// accessors as strand-side or at-rest (after io_context::run() returned).
// The two exceptions are the concurrent_channel frontends write_input() /
// close_input(), which are safe from any thread by design.
//
// The initial-wait deadline (await_initial_execution): the first wait
// races the spec's initial_wait_timeout_milliseconds against the child's
// exit. The window opens when the call begins, NOT at spawn — it is an
// initial-wait grace period, not a process lifetime budget (though under
// kill semantics it becomes one: see detach_on_timeout). 0 disables the
// deadline entirely (wait indefinitely, the answer is always true). Exit
// first => true, done. Deadline first => false, and detach_on_timeout
// decides the aftermath — true leaves the child running under a fresh
// await task that records its eventual exit; false terminates it (v2's
// terminate() is a hard kill: SIGKILL, reported by exit_code() as 9;
// request_exit() would be the graceful SIGTERM). The await task re-probes
// the child on a short cadence rather than trusting the pidfd wait alone:
// v2 arms the pidfd after a waitpid probe, and a child exiting in that gap
// would otherwise be lost to asio's edge-triggered epoll forever.
//
// Output capture is capped by the spec's max_output_bytes (shared by both
// streams, 0 = unlimited). The readers keep DRAINING past the cap and
// discard the excess — a reader that stopped would block the child on a
// full pipe — and stdout_truncated()/stderr_truncated() report which
// captured text stopped at the cap.
//

#include <array>
#include <chrono>
#include <cstdlib>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/process.hpp>
#include <boost/system.hpp>
#include <boost/process/environment.hpp>

#include "process/process_exceptions.hpp"
#include "dataclass/process_spec.hpp"
#include "logging/logger.hpp"

namespace process {

class ProcessHandle : public std::enable_shared_from_this<ProcessHandle> {
public:
    using ProcessPtr = std::unique_ptr<boost::process::process>;
    using MsgChannel = boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::string)>;
    static constexpr size_t READBUFFER_SIZE = 4096u;

private:
    // Identity + policy — the spec, stamped by the constructor with the
    // child's pid and start time. Never mutated after construction.
    LaunchSpec _spec;
    ProcessPtr _process_ptr;

    // Monotonic anchor for every duration this class computes. started_at
    // (in the spec) is the wall-clock timestamp for reports; system_clock
    // is not guaranteed monotonic (NTP jumps), so elapsed-time accounting
    // measures from here instead. Stamped next to started_at, never
    // serialized — a steady_clock point means nothing outside this process.
    std::chrono::steady_clock::time_point _started_steady{};

    // Everything below runs on this strand. The pipes: _pipe0 feeds the
    // child's stdin, _pipe1/_pipe2 drain its stdout/stderr.
    boost::asio::strand<boost::asio::any_io_executor> _strand;

    boost::asio::writable_pipe _pipe0;
    boost::asio::readable_pipe _pipe1;
    boost::asio::readable_pipe _pipe2;

    // The stdin faucet. concurrent_channel is the one thread-safe member:
    // write_input()/close_input() may be called from anywhere, everything
    // else goes through the strand.
    MsgChannel _write_channel;

    // Captured output, appended incrementally by the read tasks until the
    // spec's max_output_bytes cap (0 = no cap). The readers never stop at
    // the cap — they keep draining so the child cannot block on a full
    // pipe — they just stop KEEPING what arrived past it, and the matching
    // truncated flag goes up the first time bytes are dropped.
    std::string _standard_out;
    std::string _standard_err;
    bool _stdout_truncated = false;
    bool _stderr_truncated = false;

    // One-shot guard for the lifecycle contract below (assert-checked, not
    // a runtime state machine by design).
    bool _io_tasks_started = false;

    // Engaged exactly once, by whichever await task observes the child's
    // terminal state. Disengaged => status() reports the live view.
    std::optional<ExecutionStatus> _final_status;
    std::vector<std::future<void>> _background_tasks;

    boost::asio::awaitable<void> background_write_task(boost::asio::writable_pipe& pipe, MsgChannel& channel);
    boost::asio::awaitable<void> background_read_task(boost::asio::readable_pipe& pipe, std::string& output, bool& truncated);
    boost::asio::awaitable<void> background_await_task();

public:
    ProcessHandle(LaunchSpec spec, boost::asio::any_io_executor executor);
    ~ProcessHandle();
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator = (const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&&) = delete;
    ProcessHandle& operator = (ProcessHandle&&) = delete;

    // -- lifecycle (co_spawn these on the handle's strand) ------------------

    // Internal lifecycle contract:
    //
    // ProcessHandle is expected to be owned and orchestrated by a
    // higher-level manager. Once start_background_io_tasks() succeeds, the
    // manager must continue driving the handle through
    // await_initial_execution() and retain ownership until the child is
    // eventually observed in a terminal state. These two functions are one
    // lifecycle, not independently composable public operations — the
    // background tasks hold shared_from_this() from the first call on, so
    // an abandoned half-started handle defers its own destruction to the
    // child's natural death. (Guarded by an assert, deliberately not by a
    // runtime state machine.)
    boost::asio::awaitable<void> start_background_io_tasks();

    // Races the initial-wait deadline against the child's exit — the
    // window opens HERE, not at spawn (see the class comment). Returns
    // true if the child exited within the window (final status already
    // recorded), false if the deadline fired — in which case
    // detach_on_timeout has been applied and a fresh await task is
    // tracking the child's aftermath.
    boost::asio::awaitable<bool> await_initial_execution();

    // -- stdin control (thread-safe: the only off-strand entry points) ------

    // Queues one message for the child's stdin. Delivered in order by the
    // background pump; a send onto a closed channel is logged and dropped.
    // Fire-and-forget by design: this returns before delivery, and a full
    // channel buffer parks the SEND COMPLETION, not this call — callers
    // producing stdin faster than the child consumes it can therefore
    // queue unboundedly many outstanding sends. Manager-side input volume
    // is expected to be small and line-driven; if real backpressure is
    // ever needed, add an awaitable send rather than growing this one.
    void write_input(std::string message);

    // Ends stdin: buffered messages are still delivered (asio channels drain
    // on close), then the pump closes the pipe and the child sees EOF.
    void close_input();

    // -- observation (strand-owned state: call on the strand or at rest) ----

    [[nodiscard]] const LaunchSpec& spec() const noexcept;
    [[nodiscard]] pid_t pid() const noexcept;
    [[nodiscard]] std::chrono::system_clock::time_point started_at() const noexcept;

    // True once some await task has observed the child's terminal state.
    [[nodiscard]] bool exited() const noexcept;

    // The terminal record once exited(); before that, the live view —
    // Running with the duration accumulated off the stamped start time.
    [[nodiscard]] ExecutionStatus status() const;

    [[nodiscard]] const std::string& standard_output() const noexcept;
    [[nodiscard]] const std::string& standard_error() const noexcept;

    // True when the matching stream produced more than the spec's
    // max_output_bytes and the captured text stops at the cap (the excess
    // was drained and discarded — see the class comment).
    [[nodiscard]] bool stdout_truncated() const noexcept;
    [[nodiscard]] bool stderr_truncated() const noexcept;

    // The report-shaped view: stamped spec + current status + both captured
    // streams (always engaged — the pipes are wired for every launch, so ""
    // means captured silence, never "not captured") and their truncated
    // flags.
    [[nodiscard]] ExecutionResult snapshot() const;
};

}
