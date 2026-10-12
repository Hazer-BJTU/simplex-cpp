#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio.hpp>
#include "dataclass/process_spec.hpp"
#include "process/process_exceptions.hpp"
#include "process_io/environment.hpp"

namespace subprocess {
class Child;
}

namespace process {

/** One child lifecycle, with I/O delegated to ProcessIoEnvironment.
 * Construction synchronously launches the child and stamps its identity. The
 * constructor owns an unpublished environment, so attachment setup is exclusive.
 * Environment assembly, executable resolution and native cleanup belong to
 * utils/subprocess. The environment owns every pipe, pump, queue and output byte.
 *
 * Own this type in a shared_ptr. start_background_io_tasks() starts I/O AND its
 * child watcher transactionally; await_initial_execution() is only an optional
 * wait/deadline policy, not a prerequisite for cleanup. The watcher retains the
 * handle until child exit or explicit shutdown. Owners must call shutdown() to
 * join work, including inherited output pipes that outlive the direct child.
 *
 * A supplied strand is reused, not wrapped in another strand. Awaitable methods
 * marshal to that strand, which serializes each synchronous section while still
 * allowing coroutine interleaving at awaits. Mutable observations require the
 * strand or a quiescent context with all worker threads joined; io_snapshot()
 * explicitly requires the strand even then. Borrowed output must not survive an
 * await. Only exited() is an atomic cross-thread lifecycle observation.
 *
 * Destruction/emergency_cleanup are defensive synchronous child cleanup, not
 * asynchronous join fences. Never stop the context before normal shutdown.
 */
class ProcessHandle : public std::enable_shared_from_this<ProcessHandle> {
public:
    /// Launch synchronously with pipes or a PTY according to spec.tty. An erased
    /// executor that already contains this strand type is reused; otherwise a
    /// strand is created. Launch failures retain executable/stage context.
    ProcessHandle(LaunchSpec spec, boost::asio::any_io_executor executor);
    /// Reuse the exact owner strand so child observation and capture share it.
    ProcessHandle(LaunchSpec spec, process_io::Executor strand);
    template <class Executor>
    ProcessHandle(LaunchSpec spec, boost::asio::strand<Executor> strand)
        : ProcessHandle(std::move(spec), process_io::Executor(std::move(strand)))
    {
    }
    ~ProcessHandle();
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&&) = delete;
    ProcessHandle& operator=(ProcessHandle&&) = delete;

    /// Start once. Failure/cancellation rolls back all owned work before throwing.
    /// An initial-wait call is not required to observe or clean up the child.
    /// Repeated startup is a caller error and leaves the existing child intact.
    boost::asio::awaitable<void> start_background_io_tasks();
    /// Wait from call time, not spawn time. Zero means no deadline; otherwise
    /// return false on timeout and apply detach_on_timeout (false kills child).
    /// Cancelling this wait does not cancel the independently supervised watcher.
    /// Watcher failure returns false; shutdown reports the retained failure.
    boost::asio::awaitable<bool> await_initial_execution();

    /// Ordered acceptance, not delivery or consumption. Closed means rejected.
    /// Await this operation; input now uses the backend queue directly.
    [[nodiscard]] boost::asio::awaitable<process_io::InputResult> write_input(std::string message);
    /// Sequence a backend-dependent input-end request after all accepted input.
    /// Await before sending signals when ordering matters; acceptance does not
    /// promise child consumption. For a PTY this is canonical EOF, not half-close.
    [[nodiscard]] boost::asio::awaitable<process_io::InputResult> close_input();
    /// Signal the direct child only; the watcher records terminal observation.
    boost::asio::awaitable<bool> terminate();
    /// Request graceful direct-child exit. True reports a signal, not completion.
    boost::asio::awaitable<bool> request_exit();

    /** Cancellation-shielded, repeatable best-effort lifetime fence.
     * Stop admission, kill/reap, allow 100 ms of output drainage, then cancel and
     * join I/O and the watcher. Works before start and after partial startup.
     * Direct signals do not target descendants; final PTY close may hang them up.
     * Retain the first failure, finish all cleanup,
     * then rethrow it; failure is not represented as a successful child exit.
     */
    boost::asio::awaitable<void> shutdown();
    /// Last resort only: quiesce the context and join worker threads first.
    /// Kills/reaps using owned native state; cannot join suspended I/O tasks.
    void emergency_cleanup() noexcept;

    /// Immutable launch metadata stamped during construction; no native ownership.
    [[nodiscard]] const LaunchSpec& spec() const noexcept;
    [[nodiscard]] ProcessId pid() const noexcept;
    [[nodiscard]] std::chrono::system_clock::time_point started_at() const noexcept;
    /// Atomic, monotonic successful exit observation. Watcher failure is not exit,
    /// and true does not imply that output capture or background joins completed.
    [[nodiscard]] bool exited() const noexcept;
    /// Serialized/quiescent observation; elapsed time uses the monotonic clock.
    [[nodiscard]] ExecutionStatus status() const;
    /// Legacy pipe-only accessors; absent streams throw in TTY mode.
    /// Use capture() or snapshot() for backend-independent output.
    [[nodiscard]] const std::string& standard_output() const;
    [[nodiscard]] const std::string& standard_error() const;
    [[nodiscard]] bool stdout_truncated() const;
    [[nodiscard]] bool stderr_truncated() const;
    /// Capture ended (EOF, failure or forced stop), not necessarily joined or
    /// lossless. Distinct from exited(); shutdown is the explicit join fence.
    [[nodiscard]] bool output_drained() const noexcept;
    /// Copy the launch/status and raw retained bytes, preserving absent streams.
    /// Text decoding belongs to the tool boundary, not this host-side snapshot.
    [[nodiscard]] ExecutionResult snapshot() const;
    /// Borrow on the owning strand without suspension; absent streams stay absent.
    [[nodiscard]] process_io::Snapshot io_snapshot() const;
    [[nodiscard]] const process_io::CaptureBuffer& capture() const noexcept;

private:
    // After construction, mutable lifecycle fields belong to _strand. The atomic
    // exit latch is the sole cross-thread exception; it publishes _final_status
    // after successful observation but does not serialize other member access.
    LaunchSpec _spec;
    process_io::Executor _strand;
    std::unique_ptr<process_io::ProcessIoEnvironment> _io;
    std::unique_ptr<subprocess::Child> _process_ptr;
    std::chrono::steady_clock::time_point _started_steady{};
    bool _io_tasks_started = false;
    bool _initial_wait_started = false;
    bool _initial_wait_active = false;
    bool _stop_watcher = false;
    bool _watcher_active = false;
    std::exception_ptr _lifecycle_failure;
    std::optional<ExecutionStatus> _final_status;
    std::atomic<bool> _terminal_observed{false};

    /// Publish the successfully reaped exit status before setting the atomic latch.
    void record_terminal_state();
    /// Retain the first I/O error and add launch context when it is available.
    void remember_io_failure(std::exception_ptr failure) noexcept;
    /// Sole ordinary exit observer; periodically probes around pidfd wait races.
    boost::asio::awaitable<void> background_await_task();
    /// Start pumps and watcher together, rolling back any partially started work.
    boost::asio::awaitable<void> start_background_io_tasks_on_strand();
    /// Apply the one-shot initial-wait policy without owning the child watcher.
    boost::asio::awaitable<bool> await_initial_execution_on_strand();
    /// Shield cancellation and complete every cleanup stage before rethrowing.
    boost::asio::awaitable<void> shutdown_on_strand();
    enum class Signal {
        Kill,
        Graceful
    };
    /// Marshal signaling onto the same strand as exit observation.
    boost::asio::awaitable<bool> signal_child(Signal signal);
    /// Probe then signal the owned child; never publish exit solely from a signal.
    boost::asio::awaitable<bool> signal_child_on_strand(Signal signal);
};

} // namespace process
