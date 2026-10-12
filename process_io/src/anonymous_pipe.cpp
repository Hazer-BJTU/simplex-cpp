#include "process_io/anonymous_pipe.hpp"
#include "subprocess/pipe_writer.hpp"

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

/// Strand-owned transport and pump supervision, shared with completion handlers.
/// The facade may disappear first; no queued bytes or pump buffers borrow it.
struct AnonymousPipeIo::State : std::enable_shared_from_this<State> {
    enum class Task {
        Input,
        Output,
        Error
    };

    explicit State(Executor executor, std::size_t limit)
        : strand(std::move(executor)),
          input(strand),
          output(strand),
          error(strand),
          wake_input(strand),
          capture(OutputTopology::Separate, limit)
    {
    }

    Executor strand;
    subprocess::PipeWriter input;
    asio::readable_pipe output;
    asio::readable_pipe error;
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
    // Count spawned pumps until their completion handlers, not just bodies, finish.
    std::size_t active_tasks = 0;
    std::exception_ptr failure;

    void require_strand() const
    {
        if (!strand.running_in_this_thread()) {
            throw std::logic_error("process I/O observation/attachment requires its strand");
        }
    }

    /// Keep the first failure through later cleanup; join_tasks() reports it last.
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

    /// Nonthrowing transport close that retains errors without skipping other work.
    template <class Pipe>
    boost::system::error_code close(Pipe& pipe) noexcept
    {
        boost::system::error_code code;
        if (pipe.is_open()) {
            pipe.close(code);
            remember(code);
        }
        return code;
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
        if (close(input)) {
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
        capture.finish(Stream::Stdout, StreamEnd::Stopped);
        capture.finish(Stream::Stderr, StreamEnd::Stopped);
        close(output);
        close(error);
    }

    /// Exactly one completion callback per successfully spawned pump. Retain
    /// errors, finalize the affected stream and close its resource BEFORE
    /// decrementing the count observed by the join fence.
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
                capture.finish(
                    task == Task::Output ? Stream::Stdout : Stream::Stderr,
                    StreamEnd::Failed,
                    code);
            }
        }
        if (task == Task::Input) {
            close(input);
        } else if (task == Task::Output) {
            close(output);
        } else {
            close(error);
        }
        --active_tasks;
    }

    /// Register before scheduling so inline completion and scheduling failure
    /// both leave the join count consistent. The handler owns State until done.
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

    /// Drain even when capture is full: retention loss must not block the child.
    /// The coroutine owns its read buffer for the full asynchronous operation.
    asio::awaitable<void> read(asio::readable_pipe& pipe, Stream stream)
    {
        std::array<char, 4096> buffer;
        while (!stopping) {
            boost::system::error_code code;
            const auto count = co_await pipe.async_read_some(
                asio::buffer(buffer), asio::redirect_error(asio::use_awaitable, code));
            // abort() may have ended capture while this operation was pending.
            // Its bytes are deliberately not appended past that explicit fence.
            if (stopping) {
                co_return;
            }
            if (count != 0) {
                capture.append(stream, std::string_view(buffer.data(), count));
            }
            if (code == asio::error::eof) {
                capture.finish(stream, StreamEnd::Eof);
                co_return;
            }
            if (code) {
                throw boost::system::system_error(code);
            }
        }
    }

    /// Single writer preserves queue order. Input-end closes only after the queue
    /// drains; timer cancellation is the strand-local wakeup for newly queued work.
    asio::awaitable<void> write()
    {
        while (!stopping && !input_stopped) {
            if (queue.empty()) {
                if (end_requested) {
                    input_phase = close(input) ? InputPhase::Failed : InputPhase::Ended;
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
            // Keep this owned string in the coroutine frame while abort_input()
            // is free to discard the rest of the queue during the write await.
            queue.pop_front();
            boost::system::error_code code;
            co_await input.write(bytes, code);
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

    /// Await every completion handler before exposing joined or throwing failure.
    /// Called with cancellation disabled so teardown cannot abandon owned tasks.
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
        if (started || stopping || input_stopped || !prepared || !input.is_open()
            || !output.is_open() || !error.is_open()) {
            throw std::logic_error("start requires one successful, unused pipe attachment launch");
        }
        started = true;
        accepting = true;
        input_phase = InputPhase::Open;
        try {
            input.prepare();
            spawn(Task::Input, write());
            spawn(Task::Output, read(output, Stream::Stdout));
            spawn(Task::Error, read(error, Stream::Stderr));
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

AnonymousPipeIo::AnonymousPipeIo(Executor executor, std::size_t max_output_bytes)
    : _state(std::make_shared<State>(std::move(executor), max_output_bytes))
{
}

AnonymousPipeIo::~AnonymousPipeIo()
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

Executor AnonymousPipeIo::executor() const
{
    return _state->strand;
}

Capabilities AnonymousPipeIo::capabilities() const noexcept
{
    return {OutputTopology::Separate, false, InputEndMode::HalfClose, false};
}

subprocess::IoAttachment AnonymousPipeIo::prepare_attachment()
{
    auto& state = *_state;
    if (state.prepared || state.started || state.stopping) {
        throw std::logic_error("process I/O attachment can only be prepared once");
    }
    state.prepared = true;
    return subprocess::IoAttachment::pipes(state.input.launch_pipe(), state.output, state.error);
}

asio::awaitable<void> AnonymousPipeIo::start()
{
    co_await asio::co_spawn(
        _state->strand,
        [state = _state]() -> asio::awaitable<void> {
            co_await state->start_tasks();
        },
        asio::use_awaitable);
}

asio::awaitable<InputResult> AnonymousPipeIo::enqueue_input(std::string input)
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

asio::awaitable<InputResult> AnonymousPipeIo::request_input_end()
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

asio::awaitable<void> AnonymousPipeIo::stop_admission()
{
    co_await asio::co_spawn(
        _state->strand,
        [state = _state]() -> asio::awaitable<void> {
            state->accepting = false;
            co_return;
        },
        asio::use_awaitable);
}

asio::awaitable<void> AnonymousPipeIo::cancel_input()
{
    co_await asio::co_spawn(
        _state->strand,
        [state = _state]() -> asio::awaitable<void> {
            state->abort_input();
            co_return;
        },
        asio::use_awaitable);
}

const CaptureBuffer& AnonymousPipeIo::capture() const noexcept
{
    return _state->capture;
}

Snapshot AnonymousPipeIo::snapshot() const
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

asio::awaitable<bool> AnonymousPipeIo::drain_until(std::chrono::steady_clock::time_point deadline)
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

asio::awaitable<void> AnonymousPipeIo::cancel_and_join()
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

asio::awaitable<ResizeResult> AnonymousPipeIo::resize(unsigned, unsigned)
{
    co_return ResizeResult::Unsupported;
}

} // namespace process_io
