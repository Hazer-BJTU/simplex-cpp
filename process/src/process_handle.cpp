#include "process/process_handle.hpp"
#include "subprocess/child.hpp"
#include "process_io/anonymous_pipe.hpp"
#include "process_io/posix_pty.hpp"
#include "logging/logger.hpp"
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <format>

#include <boost/scope/scope_exit.hpp>
#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>

namespace process {

ProcessHandle::ProcessHandle(LaunchSpec spec, boost::asio::any_io_executor executor)
    : ProcessHandle(
          std::move(spec),
          executor.target<process_io::Executor>() ? *executor.target<process_io::Executor>()
                                                  : process_io::Executor(executor))
{
}

ProcessHandle::ProcessHandle(LaunchSpec spec, process_io::Executor strand)
    : _spec(std::move(spec)),
      _strand(std::move(strand)),
      _io(_spec.tty
              ? std::unique_ptr<process_io::ProcessIoEnvironment>(
                    std::make_unique<process_io::PosixPtyIo>(_strand, _spec.max_output_bytes))
              : std::make_unique<process_io::AnonymousPipeIo>(_strand, _spec.max_output_bytes))
{
    // The environment is still unpublished, so attachment preparation and launch
    // are exclusive. Child/transport RAII owns rollback if construction throws.
    const auto attachment = _io->prepare_attachment();
    _process_ptr = std::make_unique<subprocess::Child>(_spec, _strand, attachment);
    _spec.pid = _process_ptr->id();
    _spec.started_at = std::chrono::system_clock::now();
    _started_steady = std::chrono::steady_clock::now();
}

ProcessHandle::~ProcessHandle()
{
    emergency_cleanup();
}

void ProcessHandle::emergency_cleanup() noexcept
{
    if (!_process_ptr || exited()) {
        return;
    }
    const auto error = _process_ptr->emergency_cleanup();
    if (error) {
        // Cleanup itself must remain noexcept even if logging allocates.
        try {
            logging::Logger::error(std::format(
                "emergency cleanup for child {} (pid {}) failed: {}",
                _spec.executable,
                _spec.pid,
                error.message()));
        } catch (...) {
        }
    }
}

boost::asio::awaitable<void> ProcessHandle::background_await_task()
{
    using namespace boost::asio::experimental::awaitable_operators;
    // Bound on how long a missed child exit may go unnoticed — see the arm
    // race note below.
    constexpr auto probe_interval = std::chrono::milliseconds{100};

    boost::system::error_code ec;
    boost::asio::steady_timer probe{_strand};

    // Failed observation is not a terminal child status. Retain the exception
    // separately so shutdown can join this task and retry reaping without
    // waiting forever for exited(). This path attempts a defensive direct-child
    // termination; shutdown owns input cancellation and the remaining task joins.
    auto watcher_failed = [&](std::string_view what) {
        if (!_lifecycle_failure) {
            _lifecycle_failure = std::current_exception();
        }
        boost::system::error_code error;
        const bool running = _process_ptr->running(error);
        if (!error && running) {
            _process_ptr->terminate(error);
        }
        logging::Logger::error(std::format("process background await task failed: {}", what));
    };

    try {
        // The wait cannot be a plain async_wait: v2's pidfd wait probes
        // waitpid(WNOHANG) and only then arms the pidfd — a child exiting
        // in that gap is lost forever, because asio's epoll registration is
        // edge-triggered and the exit edge is never re-delivered. The
        // re-probe loop bounds the damage: any missed edge is caught by a
        // running() (waitpid WNOHANG) within one probe interval.
        bool terminal = false;
        boost::system::error_code probe_ec;
        if (!_process_ptr->running(probe_ec)) {
            // Already gone (short-lived children usually are, by the time
            // this task first runs) — reaped, status stored, no arming.
            if (probe_ec) {
                throw ProcessException(
                    ProcessException::Stage::Terminate,
                    "initial waitpid probe failed",
                    probe_ec,
                    _spec.executable,
                    _spec.description);
            }
            terminal = true;
        }

        while (!terminal) {
            // Shutdown owns recovery once this watcher has joined. The probe
            // timer bounds the wait even if a failed signal leaves a live child.
            if (_stop_watcher) {
                co_return;
            }
            probe.expires_after(probe_interval);
            ec.clear();
            auto which =
                co_await (probe.async_wait(boost::asio::use_awaitable) || _process_ptr->wait(ec));
            if (which.index() == 1) {
                // The process wait ended. An aborted native operation is not
                // terminal observation; shutdown owns recovery when stopping.
                if (ec == boost::asio::error::operation_aborted) {
                    co_return;
                }
                if (ec) {
                    throw ProcessException(
                        ProcessException::Stage::Terminate,
                        "background await task exited because of unexpected exception",
                        ec,
                        _spec.executable,
                        _spec.description);
                }
                terminal = true; // exit_status_ stored by the wait itself
            } else {
                // Probe timer won: the child may have died inside the arm
                // race window. running() reaps; still alive means loop and
                // arm fresh (which also clears a dead-armed pidfd op, since
                // || cancelled it).
                probe_ec.clear();
                if (!_process_ptr->running(probe_ec)) {
                    if (probe_ec) {
                        throw ProcessException(
                            ProcessException::Stage::Terminate,
                            "waitpid probe failed",
                            probe_ec,
                            _spec.executable,
                            _spec.description);
                    }
                    terminal = true;
                }
            }
        }

        // A failed watcher cannot publish a terminal status. Shutdown may
        // later do so only after its independent recovery probe/reap succeeds.
        record_terminal_state();
        // A dead child cannot consume more input. Cancel terminal input
        // without closing its shared output master; pipe input can finish
        // through its ordinary end-of-input path.
        if (_spec.tty) {
            co_await _io->cancel_input();
        } else {
            co_await _io->request_input_end();
        }
        co_return;
    } catch (const boost::system::system_error& e) {
        if (e.code() == boost::asio::error::operation_aborted) {
            // The probe timer can surface cancellation as a thrown abort.
            // Never publish terminal success from an interrupted observation.
            co_return;
        }
        watcher_failed(e.what());
        co_return;
    } catch (const std::exception& e) {
        watcher_failed(e.what());
        co_return;
    } catch (...) {
        watcher_failed("unknown error");
        co_return;
    }
}

void ProcessHandle::record_terminal_state()
{
    ExecutionStatus state;
    state.state = ProcessState::Exited;
    state.exit_code = _process_ptr->exit_code();
    state.cumulative_execution_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _started_steady)
            .count();
    _final_status = std::move(state);
    // The latch AFTER the optional, and that order is the contract: any
    // thread that sees exited() == true is guaranteed — by the release
    // below — to see everything the terminal observation wrote, including
    // the _final_status a strand-side status() reads.
    _terminal_observed.store(true, std::memory_order_release);
}

boost::asio::awaitable<void> ProcessHandle::start_background_io_tasks()
{
    std::exception_ptr failure;
    try {
        co_await boost::asio::co_spawn(
            _strand,
            [self = shared_from_this()]() -> boost::asio::awaitable<void> {
                co_await self->start_background_io_tasks_on_strand();
            },
            boost::asio::use_awaitable);
        co_return;
    } catch (const std::logic_error&) {
        // Repeated startup is a caller error, not permission to kill an
        // existing process. Internal startup failures already rolled back.
        throw;
    } catch (...) {
        failure = std::current_exception();
    }
    // Also covers cancellation/scheduling failure before the strand body runs.
    // Keep the original failure if cleanup independently reports another error.
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    try {
        co_await shutdown();
    } catch (...) {
    }
    std::rethrow_exception(failure);
}

void ProcessHandle::remember_io_failure(std::exception_ptr failure) noexcept
{
    if (_lifecycle_failure || !failure) {
        return;
    }
    _lifecycle_failure = failure;
    try {
        std::rethrow_exception(failure);
    } catch (const boost::system::system_error& error) {
        try {
            // A backend error may precede later failures on another stream.
            // Do not infer the first error's source from the latest input phase.
            _lifecycle_failure = std::make_exception_ptr(ProcessException(
                ProcessException::Stage::Unknown,
                "process I/O failed",
                error.code(),
                _spec.executable,
                _spec.description));
        } catch (...) {
            // Diagnostic allocation must not prevent the remaining joins/reap.
        }
    } catch (...) {
    }
}

boost::asio::awaitable<void> ProcessHandle::start_background_io_tasks_on_strand()
{
    if (_io_tasks_started || _stop_watcher) {
        throw std::logic_error("process I/O can only be started once");
    }
    _io_tasks_started = true;
    std::exception_ptr failure;
    try {
        co_await _io->start();
        _watcher_active = true;
        try {
            boost::asio::co_spawn(
                _strand,
                [self = shared_from_this()]() -> boost::asio::awaitable<void> {
                    co_await self->background_await_task();
                },
                [self = shared_from_this()](std::exception_ptr error) {
                    if (error && !self->_lifecycle_failure) {
                        self->_lifecycle_failure = error;
                    }
                    self->_watcher_active = false;
                });
        } catch (...) {
            _watcher_active = false;
            throw;
        }
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        if (!_lifecycle_failure) {
            _lifecycle_failure = failure;
        }
        co_await shutdown_on_strand();
        std::rethrow_exception(failure);
    }
}

boost::asio::awaitable<bool> ProcessHandle::await_initial_execution()
{
    // The spawned coroutine retains the strand executor after every await.
    co_return co_await boost::asio::co_spawn(
        _strand,
        [self = shared_from_this()]() -> boost::asio::awaitable<bool> {
            co_return co_await self->await_initial_execution_on_strand();
        },
        boost::asio::use_awaitable);
}

boost::asio::awaitable<bool> ProcessHandle::await_initial_execution_on_strand()
{
    if (!_io_tasks_started || _initial_wait_started) {
        throw std::logic_error("initial wait requires startup and can run only once");
    }
    _initial_wait_started = true;
    _initial_wait_active = true;
    boost::scope::scope_exit finished([this] {
        _initial_wait_active = false;
    });
    const auto deadline =
        _spec.initial_wait_timeout_milliseconds == 0
            ? std::chrono::steady_clock::time_point::max()
            : std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(_spec.initial_wait_timeout_milliseconds);
    boost::asio::steady_timer pause(_strand);
    while (!exited() && _watcher_active && !_stop_watcher && !_lifecycle_failure) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            if (!_spec.detach_on_timeout) {
                co_await signal_child_on_strand(Signal::Kill);
            }
            co_return false;
        }
        pause.expires_at(std::min(deadline, now + std::chrono::milliseconds(5)));
        co_await pause.async_wait(boost::asio::use_awaitable);
    }
    co_return exited();
}

boost::asio::awaitable<process_io::InputResult> ProcessHandle::write_input(std::string message)
{
    co_return co_await boost::asio::co_spawn(
        _strand,
        [self = shared_from_this(), message = std::move(message)]() mutable
            -> boost::asio::awaitable<process_io::InputResult> {
            if (self->exited()) {
                co_return process_io::InputResult::Closed;
            }
            co_return co_await self->_io->enqueue_input(std::move(message));
        },
        boost::asio::use_awaitable);
}

boost::asio::awaitable<process_io::InputResult> ProcessHandle::close_input()
{
    co_return co_await boost::asio::co_spawn(
        _strand,
        [self = shared_from_this()]() -> boost::asio::awaitable<process_io::InputResult> {
            co_return co_await self->_io->request_input_end();
        },
        boost::asio::use_awaitable);
}

// ---- termination ------------------------------------------------------------
//
// Both entry points are the same shape and differ only in the signal, so the
// body is shared: marshal onto the strand, probe, signal, report. What they
// deliberately do NOT do is engage _final_status — the watcher or shutdown
// recovery owns that (see the class invariant), and a signal is not an
// observation. The child's death arrives at the watcher like any other exit,
// which is what makes these composable with the ordinary lifecycle instead of
// a second, competing teardown path.

boost::asio::awaitable<void> ProcessHandle::shutdown()
{
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    co_await boost::asio::co_spawn(
        _strand,
        [self = shared_from_this()]() -> boost::asio::awaitable<void> {
            co_await self->shutdown_on_strand();
        },
        boost::asio::use_awaitable);
}

boost::asio::awaitable<void> ProcessHandle::shutdown_on_strand()
{
    co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    auto remember = [this] {
        if (!_lifecycle_failure) {
            _lifecycle_failure = std::current_exception();
        }
    };
    _stop_watcher = true;
    remember_io_failure(_io->snapshot().failure);
    try {
        co_await _io->cancel_input();
    } catch (...) {
        remember();
    }
    try {
        co_await signal_child_on_strand(Signal::Kill);
    } catch (...) {
        remember();
    }
    try {
        co_await _io->drain_until(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
    } catch (...) {
        remember();
    }
    try {
        co_await _io->cancel_and_join();
    } catch (...) {
        remember_io_failure(std::current_exception());
    }
    // No initial-wait prerequisite: also safe before start or after any partial
    // startup. Watcher completion, not successful child exit, is the join flag.
    boost::asio::steady_timer pause(_strand);
    while (_watcher_active || _initial_wait_active) {
        pause.expires_after(std::chrono::milliseconds(5));
        co_await pause.async_wait(boost::asio::use_awaitable);
    }
    try {
        boost::system::error_code error;
        const bool running = _process_ptr->running(error);
        if (!error && running) {
            _process_ptr->terminate(error);
        }
        if (error) {
            throw ProcessException(
                ProcessException::Stage::Terminate,
                "recovering child termination during shutdown failed",
                error,
                _spec.executable,
                _spec.description);
        }
        if (!exited()) {
            record_terminal_state();
        }
    } catch (...) {
        remember();
    }
    if (_lifecycle_failure) {
        std::rethrow_exception(_lifecycle_failure);
    }
}

boost::asio::awaitable<bool> ProcessHandle::terminate()
{
    co_return co_await signal_child(Signal::Kill);
}

boost::asio::awaitable<bool> ProcessHandle::request_exit()
{
    co_return co_await signal_child(Signal::Graceful);
}

boost::asio::awaitable<bool> ProcessHandle::signal_child(Signal signal)
{
    // The spawned coroutine retains the strand executor after every await.
    co_return co_await boost::asio::co_spawn(
        _strand,
        [self = shared_from_this(), signal]() -> boost::asio::awaitable<bool> {
            co_return co_await self->signal_child_on_strand(signal);
        },
        boost::asio::use_awaitable);
}

boost::asio::awaitable<bool> ProcessHandle::signal_child_on_strand(Signal signal)
{
    // Everything below is strand-side state, so the probe and the signal
    // cannot interleave with the await task's own probe.
    if (_process_ptr == nullptr) {
        co_return false; // never spawned, or already released
    }
    if (_final_status.has_value()) {
        // Already observed terminal: no pid worth signalling — and on a
        // recycled pid, signalling would hit somebody else's process.
        co_return false;
    }

    boost::system::error_code probe_ec;
    const bool running = _process_ptr->running(probe_ec);
    if (probe_ec) {
        throw ProcessException(
            ProcessException::Stage::Terminate,
            "probing the child before signalling it failed",
            probe_ec,
            _spec.executable,
            _spec.description);
    }

    if (!running) {
        co_return false;
    }

    // The non-throwing overloads on purpose: the throwing ones raise
    // boost::system::system_error, and this class's boundary is
    // ProcessException (the rule process_exceptions.hpp states).
    boost::system::error_code signal_ec;
    if (signal == Signal::Kill) {
        _process_ptr->terminate(signal_ec);
    } else {
        _process_ptr->request_exit(signal_ec);
    }
    if (signal_ec) {
        throw ProcessException(
            ProcessException::Stage::Terminate,
            signal == Signal::Kill ? "failed to terminate the process"
                                   : "failed to request the process to exit",
            signal_ec,
            _spec.executable,
            _spec.description);
    }

    logging::Logger::debug(std::format(
        "signalled child {} (pid {}) to {}",
        _spec.executable,
        _spec.pid,
        signal == Signal::Kill ? "terminate" : "exit"));
    co_return true;
}

// ---- observation ------------------------------------------------------------

const LaunchSpec& ProcessHandle::spec() const noexcept
{
    return _spec;
}

ProcessId ProcessHandle::pid() const noexcept
{
    return _spec.pid;
}

std::chrono::system_clock::time_point ProcessHandle::started_at() const noexcept
{
    return _spec.started_at;
}

bool ProcessHandle::exited() const noexcept
{
    // The latch, not _final_status: see the class comment. An acquire pairs
    // with the release in record_terminal_state(), so a caller that sees true
    // also sees whatever the terminal observation published before it.
    return _terminal_observed.load(std::memory_order_acquire);
}

ExecutionStatus ProcessHandle::status() const
{
    if (_final_status) {
        return *_final_status;
    }
    // No terminal observation yet: the handle spawned its child in the
    // constructor, so Running is the honest state — not Unknown — and the
    // duration keeps accumulating off the monotonic anchor (never
    // started_at: wall clock is for reports, not for measuring).
    ExecutionStatus live;
    live.state = ProcessState::Running;
    live.cumulative_execution_milliseconds =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - _started_steady)
                                       .count());
    return live;
}

const std::string& ProcessHandle::standard_output() const
{
    return _io->capture().bytes(process_io::Stream::Stdout);
}

const std::string& ProcessHandle::standard_error() const
{
    return _io->capture().bytes(process_io::Stream::Stderr);
}

bool ProcessHandle::stdout_truncated() const
{
    return _io->capture().view(process_io::Stream::Stdout).value().truncated;
}

bool ProcessHandle::stderr_truncated() const
{
    return _io->capture().view(process_io::Stream::Stderr).value().truncated;
}

bool ProcessHandle::output_drained() const noexcept
{
    return _io->capture().complete();
}

process_io::Snapshot ProcessHandle::io_snapshot() const
{
    return _io->snapshot();
}
const process_io::CaptureBuffer& ProcessHandle::capture() const noexcept
{
    return _io->capture();
}

ExecutionResult ProcessHandle::snapshot() const
{
    ExecutionResult result;
    result.spec = _spec;
    result.execution = status();
    const auto& capture = _io->capture();
    if (const auto stream = capture.view(process_io::Stream::Stdout)) {
        result.stdout_text = stream->bytes;
        result.stdout_truncated = stream->truncated;
    }
    if (const auto stream = capture.view(process_io::Stream::Stderr)) {
        result.stderr_text = stream->bytes;
        result.stderr_truncated = stream->truncated;
    }
    if (const auto stream = capture.view(process_io::Stream::Terminal)) {
        result.terminal_text = stream->bytes;
        result.terminal_truncated = stream->truncated;
    }
    return result;
}

}
