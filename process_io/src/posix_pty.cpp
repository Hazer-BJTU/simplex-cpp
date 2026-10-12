#include "process_io/posix_pty.hpp"
#include "subprocess/terminal.hpp"

#include <array>
#include <deque>
#include <stdexcept>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>

namespace process_io {
namespace asio = boost::asio;

/// One strand owns terminal state, capture and both supervised pumps.
/// Completion handlers retain this state independently of the public facade.
struct PosixPtyIo::State : std::enable_shared_from_this<State> {
    enum class Task {
        Input,
        Output
    };

    explicit State(Executor executor, std::size_t limit)
        : strand(std::move(executor)),
          terminal(strand),
          wake_input(strand),
          capture(OutputTopology::Terminal, limit)
    {
    }

    Executor strand;
    subprocess::Terminal terminal;
    asio::steady_timer wake_input;
    CaptureBuffer capture;
    // Explicitly unbounded, like the previous pending async_send population.
    // One strand defines acceptance order; input-end follows this entire queue.
    std::deque<std::string> queue;
    InputPhase input_phase = InputPhase::NotStarted;
    // Attachment preparation and task startup are separate one-shot transitions.
    bool prepared = false;
    bool started = false;
    bool accepting = false;
    bool end_requested = false;
    bool stopping = false;
    bool input_stopped = false;
    bool joined = false;
    // Count completion handlers still outstanding, including failed pump bodies.
    std::size_t active_tasks = 0;
    std::exception_ptr failure;

    void require_strand() const
    {
        if (!strand.running_in_this_thread()) {
            throw std::logic_error("process I/O observation/attachment requires its strand");
        }
    }

    /// Keep the first failure until the explicit join fence has finished cleanup.
    void remember(std::exception_ptr value) noexcept
    {
        if (!failure) {
            failure = std::move(value);
        }
    }

    void remember(boost::system::error_code code) noexcept
    {
        if (!code || failure) {
            return;
        }
        try {
            throw boost::system::system_error(code);
        } catch (...) {
            remember(std::current_exception());
        }
    }

    /// Stop input without ending output capture. No signal is sent to the child.
    void abort_input() noexcept
    {
        input_stopped = true;
        accepting = false;
        queue.clear();
        if (input_phase == InputPhase::Open || input_phase == InputPhase::EndRequested
            || input_phase == InputPhase::NotStarted) {
            input_phase = InputPhase::Stopped;
        }
        boost::system::error_code code;
        terminal.cancel_input(code);
        remember(code);
        if (code) {
            input_phase = InputPhase::Failed;
        }
        try {
            wake_input.cancel();
        } catch (...) {
            remember(std::current_exception());
        }
    }

    /// Strand-only, idempotent. Mark forced capture endings before closing
    /// descriptors, while in-flight buffers remain alive until their tasks join.
    void abort() noexcept
    {
        stopping = true;
        abort_input();
        capture.finish(Stream::Terminal, StreamEnd::Stopped);
        boost::system::error_code code;
        terminal.close(code);
        remember(code);
    }

    /// Exactly one completion callback per successfully spawned pump. Retain
    /// errors and finalize stream/input state before decrementing the count
    /// observed by the join fence. The read master stays owned until final close.
    void completed(Task task, std::exception_ptr exception)
    {
        if (exception) {
            remember(exception);
            auto code = boost::system::errc::make_error_code(boost::system::errc::io_error);
            try {
                std::rethrow_exception(exception);
            } catch (const boost::system::system_error& value) {
                code = value.code();
            } catch (...) {
            }
            if (task == Task::Input) {
                input_phase = InputPhase::Failed;
                accepting = false;
                queue.clear();
            } else {
                capture.finish(Stream::Terminal, StreamEnd::Failed, code);
            }
        }
        if (task == Task::Input) {
            boost::system::error_code code;
            terminal.cancel_input(code);
            remember(code);
        } else {
            // A closed/failed peer cannot consume pending writes. Wake an idle
            // input pump too, so natural terminal EOF leaves no orphaned task.
            const auto previous = input_phase;
            abort_input();
            if (!exception
                && (previous == InputPhase::Open || previous == InputPhase::EndRequested)) {
                input_phase = InputPhase::PeerClosed;
            }
        }
        --active_tasks;
    }

    /// Account for the pump before scheduling and undo only a scheduling failure.
    /// The completion handler retains State until its final bookkeeping is done.
    void spawn(Task task, asio::awaitable<void> operation)
    {
        ++active_tasks;
        try {
            asio::co_spawn(
                strand,
                std::move(operation),
                [self = shared_from_this(), task](std::exception_ptr exception) {
                    self->completed(task, exception);
                });
        } catch (...) {
            --active_tasks;
            throw;
        }
    }

    /// Drain one merged raw stream, including after the retention budget fills.
    /// Native PTY EOF normalization is confined to subprocess::Terminal.
    asio::awaitable<void> read()
    {
        std::array<char, 4096> buffer;
        while (!stopping) {
            boost::system::error_code code;
            const auto count = co_await terminal.read(asio::buffer(buffer), code);
            // abort() may have ended capture while this operation was pending.
            // Its bytes are deliberately not appended past that explicit fence.
            if (stopping) {
                co_return;
            }
            if (count != 0) {
                capture.append(Stream::Terminal, std::string_view(buffer.data(), count));
            }
            if (code == asio::error::eof) {
                capture.finish(Stream::Terminal, StreamEnd::Eof);
                co_return;
            }
            if (code) {
                throw boost::system::system_error(code);
            }
        }
    }

    /// Deliver accepted input in order. Once it drains, inspect the current
    /// terminal mode and perform EOF if supported; output remains independent.
    asio::awaitable<void> write()
    {
        while (!stopping && !input_stopped) {
            if (queue.empty()) {
                if (end_requested) {
                    // Queue admission cannot promise EOF support: the child may
                    // have changed termios since request_input_end() returned.
                    const auto eof = terminal.eof_request();
                    if (!eof) {
                        input_phase = InputPhase::Unsupported;
                        co_return;
                    }
                    boost::system::error_code code;
                    co_await terminal.write(*eof, code);
                    if (stopping || input_stopped) {
                        co_return;
                    }
                    if (code == asio::error::eof) {
                        input_phase = InputPhase::PeerClosed;
                    } else if (code) {
                        throw boost::system::system_error(code);
                    } else {
                        input_phase = InputPhase::Ended;
                    }
                    co_return;
                }
                wake_input.expires_at(std::chrono::steady_clock::time_point::max());
                boost::system::error_code code;
                co_await wake_input.async_wait(asio::redirect_error(asio::use_awaitable, code));
                if (code && code != asio::error::operation_aborted) {
                    throw boost::system::system_error(code);
                }
                continue;
            }
            auto bytes = std::move(queue.front());
            // Own the in-flight buffer even if input cancellation clears the queue.
            queue.pop_front();
            boost::system::error_code code;
            co_await terminal.write(bytes, code);
            if (stopping || input_stopped) {
                co_return;
            }
            if (code == asio::error::broken_pipe || code == asio::error::eof) {
                input_phase = InputPhase::PeerClosed;
                accepting = false;
                queue.clear();
                co_return;
            }
            if (code) {
                throw boost::system::system_error(code);
            }
        }
    }

    /// Wait for all pump handlers, then publish joined and report retained failure.
    /// The caller shields cancellation so input/output ownership cannot be stranded.
    asio::awaitable<void> join_tasks()
    {
        asio::steady_timer pause(strand);
        while (active_tasks != 0) {
            pause.expires_after(std::chrono::milliseconds(5));
            co_await pause.async_wait(asio::use_awaitable);
        }
        joined = true;
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    asio::awaitable<void> start_tasks()
    {
        // Startup has no suspension until all pumps are supervised. A partial
        // scheduling failure aborts and joins whatever was already spawned.
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        if (started || stopping || input_stopped || !prepared || !terminal.launched()) {
            throw std::logic_error(
                "start requires one successful, unused terminal attachment launch");
        }
        started = true;
        accepting = true;
        input_phase = InputPhase::Open;
        try {
            spawn(Task::Input, write());
            spawn(Task::Output, read());
        } catch (...) {
            remember(std::current_exception());
            abort();
        }
        if (failure) {
            abort();
            co_await join_tasks();
        }
    }
};

PosixPtyIo::PosixPtyIo(Executor executor, std::size_t max_output_bytes)
    : _state(std::make_shared<State>(std::move(executor), max_output_bytes))
{
}

PosixPtyIo::~PosixPtyIo()
{
    // Dispatch, not direct descriptor access from an arbitrary caller thread.
    // Shared state keeps buffers alive through outstanding completion handlers.
    try {
        asio::dispatch(_state->strand, [state = _state] {
            state->abort();
        });
    } catch (...) {
        // If scheduling itself fails, running tasks/context teardown retain
        // resource ownership. This fallback is deliberately not a join fence.
    }
}

Executor PosixPtyIo::executor() const
{
    return _state->strand;
}

Capabilities PosixPtyIo::capabilities() const noexcept
{
    return {OutputTopology::Terminal, true, InputEndMode::TerminalEof, true};
}

subprocess::IoAttachment PosixPtyIo::prepare_attachment()
{
    auto& state = *_state;
    if (state.prepared || state.started || state.stopping) {
        throw std::logic_error("process I/O attachment can only be prepared once");
    }
    state.prepared = true;
    return state.terminal.attachment();
}

asio::awaitable<void> PosixPtyIo::start()
{
    co_await asio::co_spawn(
        _state->strand,
        [state = _state]() -> asio::awaitable<void> {
            co_await state->start_tasks();
        },
        asio::use_awaitable);
}

asio::awaitable<InputResult> PosixPtyIo::enqueue_input(std::string input)
{
    co_return co_await asio::co_spawn(
        _state->strand,
        [state = _state, input = std::move(input)]() mutable -> asio::awaitable<InputResult> {
            if (!state->accepting) {
                co_return InputResult::Closed;
            }
            state->queue.push_back(std::move(input));
            state->wake_input.cancel();
            co_return InputResult::Accepted;
        },
        asio::use_awaitable);
}

asio::awaitable<InputResult> PosixPtyIo::request_input_end()
{
    co_return co_await asio::co_spawn(
        _state->strand,
        [state = _state]() -> asio::awaitable<InputResult> {
            if (state->input_phase != InputPhase::Open || state->stopping) {
                co_return InputResult::Closed;
            }
            state->accepting = false;
            state->end_requested = true;
            state->input_phase = InputPhase::EndRequested;
            state->wake_input.cancel();
            co_return InputResult::Accepted;
        },
        asio::use_awaitable);
}

asio::awaitable<void> PosixPtyIo::stop_admission()
{
    co_await asio::co_spawn(
        _state->strand,
        [state = _state]() -> asio::awaitable<void> {
            state->accepting = false;
            co_return;
        },
        asio::use_awaitable);
}

asio::awaitable<void> PosixPtyIo::cancel_input()
{
    co_await asio::co_spawn(
        _state->strand,
        [state = _state]() -> asio::awaitable<void> {
            state->abort_input();
            co_return;
        },
        asio::use_awaitable);
}

const CaptureBuffer& PosixPtyIo::capture() const noexcept
{
    return _state->capture;
}

Snapshot PosixPtyIo::snapshot() const
{
    const auto& state = *_state;
    state.require_strand();
    return {
        state.capture.view(Stream::Stdout),
        state.capture.view(Stream::Stderr),
        state.capture.view(Stream::Terminal),
        state.input_phase,
        state.accepting,
        state.end_requested,
        state.capture.complete(),
        state.joined,
        state.failure};
}

asio::awaitable<bool> PosixPtyIo::drain_until(std::chrono::steady_clock::time_point deadline)
{
    co_return co_await asio::co_spawn(
        _state->strand,
        [state = _state, deadline]() -> asio::awaitable<bool> {
            asio::steady_timer pause(state->strand);
            while (!state->capture.complete()) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    co_return false;
                }
                pause.expires_at(std::min(deadline, now + std::chrono::milliseconds(5)));
                co_await pause.async_wait(asio::use_awaitable);
            }
            co_return true;
        },
        asio::use_awaitable);
}

asio::awaitable<void> PosixPtyIo::cancel_and_join()
{
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    co_await asio::co_spawn(
        _state->strand,
        [state = _state]() -> asio::awaitable<void> {
            co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
            state->abort();
            co_await state->join_tasks();
        },
        asio::use_awaitable);
}

asio::awaitable<ResizeResult> PosixPtyIo::resize(unsigned columns, unsigned rows)
{
    co_return co_await asio::co_spawn(
        _state->strand,
        [state = _state, columns, rows]() -> asio::awaitable<ResizeResult> {
            state->terminal.resize(columns, rows);
            co_return ResizeResult::Applied;
        },
        asio::use_awaitable);
}

} // namespace process_io
