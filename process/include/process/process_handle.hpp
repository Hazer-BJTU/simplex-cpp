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

namespace subprocess { class Child; }

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
 * marshal to that strand. Observations run on the same strand, or after the
 * context and all worker threads have quiesced. Borrowed output must not survive
 * an await. Only exited() is an atomic cross-thread observation.
 *
 * Destruction/emergency_cleanup are defensive synchronous child cleanup, not
 * asynchronous join fences. Never stop the context before normal shutdown.
 */
class ProcessHandle : public std::enable_shared_from_this<ProcessHandle> {
public:
    ProcessHandle(LaunchSpec spec, boost::asio::any_io_executor executor);
    ProcessHandle(LaunchSpec spec, process_io::Executor strand);
    template<class Executor>
    ProcessHandle(LaunchSpec spec, boost::asio::strand<Executor> strand)
        : ProcessHandle(std::move(spec), process_io::Executor(std::move(strand))) {}
    ~ProcessHandle();
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&&) = delete;
    ProcessHandle& operator=(ProcessHandle&&) = delete;

    /// Start once. Failure/cancellation rolls back all owned work before throwing.
    /// An initial-wait call is not required to observe or clean up the child.
    boost::asio::awaitable<void> start_background_io_tasks();
    /// Wait from call time, not spawn time. Zero means no deadline; otherwise
    /// return false on timeout and apply detach_on_timeout (false kills child).
    /// Cancelling this wait does not cancel the independently supervised watcher.
    /// Watcher failure returns false; shutdown reports the retained failure.
    boost::asio::awaitable<bool> await_initial_execution();

    /// Ordered acceptance, not delivery or consumption. Closed means rejected.
    /// Await this operation; input now uses the backend queue directly.
    [[nodiscard]] boost::asio::awaitable<process_io::InputResult> write_input(std::string message);
    /// Sequence pipe EOF after all accepted input. Await before sending signals
    /// when ordering matters; acceptance does not promise child consumption.
    [[nodiscard]] boost::asio::awaitable<process_io::InputResult> close_input();
    /// Signal the direct child only; the watcher records terminal observation.
    boost::asio::awaitable<bool> terminate();
    boost::asio::awaitable<bool> request_exit();

    /** Cancellation-shielded, repeatable best-effort lifetime fence.
     * Stop admission, kill/reap, allow 100 ms of output drainage, then cancel and
     * join I/O and the watcher. Works before start and after partial startup.
     * Descendants are not killed. Retain the first failure, finish all cleanup,
     * then rethrow it; failure is not represented as a successful child exit.
     */
    boost::asio::awaitable<void> shutdown();
    /// Last resort only: quiesce the context and join worker threads first.
    /// Kills/reaps using owned native state; cannot join suspended I/O tasks.
    void emergency_cleanup() noexcept;

    [[nodiscard]] const LaunchSpec& spec() const noexcept;
    [[nodiscard]] ProcessId pid() const noexcept;
    [[nodiscard]] std::chrono::system_clock::time_point started_at() const noexcept;
    [[nodiscard]] bool exited() const noexcept;
    [[nodiscard]] ExecutionStatus status() const;
    [[nodiscard]] const std::string& standard_output() const;
    [[nodiscard]] const std::string& standard_error() const;
    [[nodiscard]] bool stdout_truncated() const;
    [[nodiscard]] bool stderr_truncated() const;
    /// Capture ended (EOF, failure or forced stop), not necessarily joined or
    /// lossless. Distinct from exited(); shutdown is the explicit join fence.
    [[nodiscard]] bool output_drained() const noexcept;
    [[nodiscard]] ExecutionResult snapshot() const;

private:
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

    void record_terminal_state();
    void remember_io_failure(std::exception_ptr failure) noexcept;
    boost::asio::awaitable<void> background_await_task();
    boost::asio::awaitable<void> start_background_io_tasks_on_strand();
    boost::asio::awaitable<bool> await_initial_execution_on_strand();
    boost::asio::awaitable<void> shutdown_on_strand();
    enum class Signal { Kill, Graceful };
    boost::asio::awaitable<bool> signal_child(Signal signal);
    boost::asio::awaitable<bool> signal_child_on_strand(Signal signal);
};

} // namespace process
