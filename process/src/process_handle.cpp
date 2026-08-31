#include "process/process_handle.hpp"

#include <chrono>

namespace process {

ProcessHandle::ProcessHandle(
    LaunchSpec spec,
    boost::asio::any_io_executor executor
): _spec(std::move(spec)),
   _process_ptr(),
   _strand(std::move(executor)),
   _pipe0(_strand),
   _pipe1(_strand),
   _pipe2(_strand),
   // Buffered on purpose: a full buffer applies backpressure to
   // write_input() senders instead of parking them, and — the real reason
   // — close() on an UNBUFFERED channel abandons parked senders' payloads,
   // while a buffered one drains what was sent before turning closed.
   _write_channel(_strand, 16),
   _standard_out(),
   _standard_err(),
   _final_status(),
   _background_tasks()
{
    // find_executable reports "not found" as an empty path rather than an
    // error, so the check has to happen before the spawn try-block.
    auto exec_path = boost::process::environment::find_executable(_spec.executable);
    if (exec_path.empty()) {
        throw ProcessException(
            ProcessException::Stage::ResolveExecutable,
            "executable not found",
            {},
            _spec.executable,
            _spec.description
        );
    }

    // execve-style environment: the parent's entries verbatim, or nothing at
    // all when the spec opts out of inheritance, then the spec's KEY=VALUE
    // entries appended — later entries win, same as a real environ.
    auto current_env = boost::process::environment::current();
    std::vector<boost::process::environment::key_value_pair> used_envs{current_env.begin(), current_env.end()};
    if (!_spec.inherit_environment) {
        used_envs.clear();
    }

    if (_spec.environment) {
        for (const auto& env_config: *_spec.environment) {
            used_envs.push_back(env_config);
        }
    }

    try {
        _process_ptr = std::make_unique<boost::process::process>(
            _strand,
            std::move(exec_path),
            _spec.arguments,
            boost::process::process_stdio{_pipe0, _pipe1, _pipe2},
            boost::process::process_environment(used_envs)
        );

        _spec.pid = _process_ptr->id();
        _spec.started_at = std::chrono::system_clock::now();
    } catch(const std::exception& e) {
        throw ProcessException(
            ProcessException::Stage::Spawn,
            e.what(),
            {},
            _spec.executable,
            _spec.description
        );
    } catch(...) {
        throw ProcessException(
            ProcessException::Stage::Spawn,
            "unknown",
            {},
            _spec.executable,
            _spec.description
        );
    }
}

ProcessHandle::~ProcessHandle()
{
    // The defensive tail — see the class comment for why teardown happens
    // before this point. The background tasks hold shared_from_this()
    // references, so by the time an owner's last reference drops, they have
    // all completed and nothing races with these closes. What can still be
    // open: the channel (an owner that never fed stdin), the pipes (a
    // detached child torn down without close_input()), and the child itself.
    boost::system::error_code ec;
    _write_channel.close();
    if (_pipe0.is_open()) _pipe0.close(ec);
    if (_pipe1.is_open()) _pipe1.close(ec);
    if (_pipe2.is_open()) _pipe2.close(ec);

    if (_process_ptr) {
        // running() is a waitpid(WNOHANG): an exited-unobserved child is
        // reaped on the way past, so no zombie lingers. A child still
        // alive at destruction is force-terminated: detach is a
        // within-lifetime concept (the deadline policy), and destroying a
        // handle whose child lives is an abandonment this class resolves by
        // killing it, not by leaking it. terminate(ec) SIGKILLs, reaps and
        // stores the exit status.
        boost::system::error_code run_ec;
        if (_process_ptr->running(run_ec)) {
            logging::Logger::warning(std::format(
                "process handle destroyed while child {} (pid {}) is still "
                "running; terminating it",
                _spec.executable, static_cast<int>(_spec.pid)));
            _process_ptr->terminate(run_ec);
        }
    }
}

// ---- background tasks -------------------------------------------------------
//
// All three share the same error philosophy: EXPECTED ends return quietly,
// unexpected ones become ProcessException and end the task with an error
// log. Expected for a stdin pump: the channel closed after draining
// (close_input() or the await task shutting down stdin post-exit), EPIPE
// (the child exited without reading the rest of its stdin), the pipe torn
// down. Expected for an output drainer: EOF — every healthy child closes
// its std{out,err} eventually. Expected for the awaiter: cancellation by
// the deadline race.
//
// Everything is caught inside each task: a background task must never leak
// an exception into the void of its use_future, where nobody is listening.

boost::asio::awaitable<void> ProcessHandle::background_write_task(
    boost::asio::writable_pipe& pipe,
    MsgChannel& channel
) {
    boost::system::error_code ec;
    try {
        while(true) {
            if (!pipe.is_open()) {
                // Torn down elsewhere — nothing left to feed.
                co_return;
            }

            std::string msg = co_await channel.async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
            if (ec) {
                // The channel is the stdin faucet. On close_input() (or the
                // await task's post-exit shutdown) asio lets the buffer
                // drain first, so arriving here means delivery is finished:
                // close our end and let the child see EOF.
                if (pipe.is_open()) {
                    boost::system::error_code close_ec;
                    pipe.close(close_ec);
                }
                if (ec == boost::asio::error::operation_aborted || !channel.is_open()) {
                    co_return; // requested teardown — not an error
                }
                throw ProcessException(
                    ProcessException::Stage::Write,
                    "process background task message queue receive error",
                    ec,
                    _spec.executable,
                    _spec.description
                );
            }

            co_await boost::asio::async_write(
                pipe,
                boost::asio::buffer(msg),
                boost::asio::redirect_error(boost::asio::use_awaitable, ec)
            );
            if (ec) {
                if (pipe.is_open()) {
                    boost::system::error_code close_ec;
                    pipe.close(close_ec);
                }
                // EPIPE and eof mean the child is gone without draining
                // stdin — the normal fate of a piped child, not a failure.
                if (ec == boost::asio::error::broken_pipe ||
                    ec == boost::asio::error::eof ||
                    ec == boost::asio::error::operation_aborted) {
                    co_return;
                }
                throw ProcessException(
                    ProcessException::Stage::Write,
                    "process background task pipe write error",
                    ec,
                    _spec.executable,
                    _spec.description
                );
            }
        }
    } catch(const std::exception& e) {
        logging::Logger::error(std::format("process background task exited: {}", e.what()));
    } catch(...) {
        logging::Logger::error("process background task exited: unknown error");
    }
    co_return;
}

boost::asio::awaitable<void> ProcessHandle::background_read_task(
    boost::asio::readable_pipe& pipe,
    std::string& output
) {
    boost::system::error_code ec;
    std::array<char, READBUFFER_SIZE> buffer;
    try {
        while(true) {
            if (!pipe.is_open()) {
                // Torn down elsewhere — nothing left to drain.
                co_return;
            }

            size_t readed_size = co_await pipe.async_read_some(
                boost::asio::buffer(buffer, buffer.size()),
                boost::asio::redirect_error(boost::asio::use_awaitable, ec)
            );
            if (readed_size) {
                output.append(buffer.data(), readed_size);
            }
            if (ec) {
                if (pipe.is_open()) {
                    boost::system::error_code close_ec;
                    pipe.close(close_ec);
                }
                // EOF is the normal end of every child — the writer closed
                // its end by exiting. Aborted is our own teardown.
                if (ec == boost::asio::error::eof ||
                    ec == boost::asio::error::operation_aborted) {
                    co_return;
                }
                throw ProcessException(
                    ProcessException::Stage::Read,
                    "process background task pipe read error",
                    ec,
                    _spec.executable,
                    _spec.description
                );
            }
        }
    } catch(const std::exception& e) {
        logging::Logger::error(std::format("process background task exited: {}", e.what()));
    } catch(...) {
        logging::Logger::error("process background task exited: unknown error");
    }
    co_return;
}

boost::asio::awaitable<void> ProcessHandle::background_await_task() {
    using namespace boost::asio::experimental::awaitable_operators;
    // Bound on how long a missed child exit may go unnoticed — see the arm
    // race note below.
    constexpr auto probe_interval = std::chrono::milliseconds{100};

    boost::system::error_code ec;
    ExecutionStatus state;
    boost::asio::steady_timer probe{_strand};
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
                    _spec.description
                );
            }
            terminal = true;
        }

        while (!terminal) {
            probe.expires_after(probe_interval);
            ec.clear();
            auto which = co_await (
                probe.async_wait(boost::asio::use_awaitable) ||
                _process_ptr->async_wait(
                    boost::asio::redirect_error(boost::asio::use_awaitable, ec)));
            if (which.index() == 1) {
                // The process wait ended. Aborted means the deadline race
                // cancelled us — the restarted instance owns the final
                // status from here on, so leave _final_status untouched.
                if (ec == boost::asio::error::operation_aborted) {
                    co_return;
                }
                if (ec) {
                    throw ProcessException(
                        ProcessException::Stage::Terminate,
                        "background await task exited because of unexpected exception",
                        ec,
                        _spec.executable,
                        _spec.description
                    );
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
                            _spec.description
                        );
                    }
                    terminal = true;
                }
            }
        }

        state.state = ProcessState::Exited;
        state.exit_code = _process_ptr->exit_code();
        auto ended_at = std::chrono::system_clock::now();
        state.cumulative_execution_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            ended_at - _spec.started_at
        ).count();
    } catch(const boost::system::system_error& e) {
        if (e.code() == boost::asio::error::operation_aborted) {
            // Cancellation arriving through the probe timer (the side
            // without redirect_error surfaces it as a thrown abort).
            co_return;
        }
        logging::Logger::error(std::format("process background task exited: {}", e.what()));
    } catch(const std::exception& e) {
        logging::Logger::error(std::format("process background task exited: {}", e.what()));
    } catch(...) {
        logging::Logger::error("process background task exited: unknown error");
    }
    _final_status = state;
    // A dead child cannot read: close the stdin faucet so the write task
    // drains and finishes too. This is what lets a handle quiesce without
    // the owner remembering to close_input() first.
    _write_channel.close();
    co_return;
}

boost::asio::awaitable<void> ProcessHandle::start_background_io_tasks() {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    auto stdin_write_task = boost::asio::co_spawn(
        _strand,
        [self = shared_from_this()]() -> boost::asio::awaitable<void> {
            co_await self->background_write_task(self->_pipe0, self->_write_channel);
        },
        boost::asio::use_future
    );

    auto stdout_read_task = boost::asio::co_spawn(
        _strand,
        [self = shared_from_this()]() -> boost::asio::awaitable<void> {
            co_await self->background_read_task(self->_pipe1, self->_standard_out);
        },
        boost::asio::use_future
    );

    auto stderr_read_task = boost::asio::co_spawn(
        _strand,
        [self = shared_from_this()]() -> boost::asio::awaitable<void> {
            co_await self->background_read_task(self->_pipe2, self->_standard_err);
        },
        boost::asio::use_future
    );

    _background_tasks.push_back(std::move(stdin_write_task));
    _background_tasks.push_back(std::move(stdout_read_task));
    _background_tasks.push_back(std::move(stderr_read_task));
    co_return;
}

boost::asio::awaitable<bool> ProcessHandle::await_initial_execution() {
    using namespace boost::asio::experimental::awaitable_operators;
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);

    if (_spec.timeout_milliseconds == 0) {
        // 0 disables the deadline: no timer to race against, just wait for
        // the terminal observation. "Finished on initial await" is then
        // always true — the wait itself is unbounded.
        co_await background_await_task();
        co_return true;
    }

    boost::asio::steady_timer timer{_strand};
    timer.expires_after(std::chrono::milliseconds(_spec.timeout_milliseconds));
    std::variant<std::monostate, std::monostate> await_result = co_await (
        timer.async_wait(boost::asio::use_awaitable) ||
        background_await_task() // Cancelled on timeout.
    );

    bool finished_on_initial_await;
    if (await_result.index() == 0) {
        finished_on_initial_await = false;
        // Deadline fired; keep watching the aftermath in the background.
        auto await_task = boost::asio::co_spawn(
            _strand,
            [self = shared_from_this()]() -> boost::asio::awaitable<void> {
                co_await self->background_await_task();
            },
            boost::asio::use_future
        );
        _background_tasks.push_back(std::move(await_task));

        if (!_spec.detach_on_timeout) {
            // terminate() is v2's hard kill (SIGKILL; request_exit() would
            // be the graceful SIGTERM). The restarted await task records the
            // signal death — exit_code() == 9 — and closes the stdin
            // channel.
            _process_ptr->terminate();
        }
        // detach_on_timeout: leave the child running; the restarted await
        // task still records its eventual natural exit.
    } else if (await_result.index() == 1) {
        finished_on_initial_await = true;
        // The await task that just won closed the stdin channel already —
        // nothing left for this handle to do but be observed.
    }

    co_return finished_on_initial_await;
}

// ---- stdin control ----------------------------------------------------------

void ProcessHandle::write_input(std::string message) {
    // concurrent_channel: safe from any thread, ordered with everything the
    // strand-side pump does. The handler exists only to notice a send onto
    // an already-closed channel (message dropped, logged) — delivery itself
    // belongs to the pump.
    _write_channel.async_send(
        boost::system::error_code{},
        std::move(message),
        [](const boost::system::error_code& send_ec) {
            if (send_ec) {
                logging::Logger::warning(std::format(
                    "dropping stdin message: channel send failed ({})",
                    send_ec.message()));
            }
        });
}

void ProcessHandle::close_input() {
    // Only the channel — deliberately NOT the pipe. asio drains a closed
    // channel's buffer to receivers, so the pump delivers whatever was
    // queued and then closes the pipe itself when the receive ends closed.
    // Closing the pipe from here would race the pump's in-flight write.
    _write_channel.close();
}

// ---- observation ------------------------------------------------------------

const LaunchSpec& ProcessHandle::spec() const noexcept {
    return _spec;
}

pid_t ProcessHandle::pid() const noexcept {
    return _spec.pid;
}

std::chrono::system_clock::time_point ProcessHandle::started_at() const noexcept {
    return _spec.started_at;
}

bool ProcessHandle::exited() const noexcept {
    return _final_status.has_value();
}

ExecutionStatus ProcessHandle::status() const {
    if (_final_status) {
        return *_final_status;
    }
    // No terminal observation yet: the handle spawned its child in the
    // constructor, so Running is the honest state — not Unknown — and the
    // duration keeps accumulating off the stamped anchor.
    ExecutionStatus live;
    live.state = ProcessState::Running;
    live.cumulative_execution_milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - _spec.started_at
        ).count());
    return live;
}

const std::string& ProcessHandle::standard_output() const noexcept {
    return _standard_out;
}

const std::string& ProcessHandle::standard_error() const noexcept {
    return _standard_err;
}

ExecutionResult ProcessHandle::snapshot() const {
    ExecutionResult result;
    result.spec = _spec;
    result.execution = status();
    result.stdout_text = _standard_out;
    result.stderr_text = _standard_err;
    return result;
}

}
