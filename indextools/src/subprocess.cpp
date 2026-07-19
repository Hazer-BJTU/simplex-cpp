#include "indextools/subprocess.hpp"
#include "indextools/schema.hpp"

namespace indextools {

// ============================================================================
// SubProcessInstance
// ============================================================================

SubProcessManager::SubProcessInstance::SubProcessInstance(
    ProcessID id,
    std::string_view description,
    std::string_view exec_name,
    std::vector<std::string> args,
    boost::asio::any_io_executor io_executor
): id(id),
   description(description),
   start_point(Clock::now()),
   exited(false),
   end_point(),
   exit_code(),
   _strand(std::move(io_executor)),
   _process_ptr(nullptr),
   _pipe0(_strand),
   _pipe1(_strand),
   _pipe2(_strand),
   _write_channel(_strand),
   _buffer1(),
   _buffer2(),
   _cursor1(0u),
   _cursor2(0u)
{
    // Resolve the executable through PATH and spawn the child with its three
    // stdio pipes wired to our asio pipes. The child inherits the parent's
    // environment so it behaves like a normal interactive process.
    auto curr_env = boost::process::environment::current();
    auto exec_path = boost::process::environment::find_executable(exec_name);
    _process_ptr = std::make_unique<boost::process::process>(
        _strand,
        std::move(exec_path),
        std::move(args),
        boost::process::process_stdio{_pipe0, _pipe1, _pipe2},
        boost::process::process_environment(curr_env)
    );
}

// On destruction, make sure we don't orphan a still-running child: send
// SIGTERM if it is alive. We intentionally do NOT async_wait() here (the
// destructor is synchronous); done() is the proper reaping path, and the
// background tasks keep the instance alive until reaping completes.
SubProcessManager::SubProcessInstance::~SubProcessInstance() {
    boost::system::error_code ec;
    if (_process_ptr && _process_ptr->running()) {
        _process_ptr->terminate(ec);
        _process_ptr.reset();
        // _process_ptr->wait(ec);
    }
    return;
}

std::shared_ptr<SubProcessManager::SubProcessInstance> SubProcessManager::SubProcessInstance::create(
    ProcessID id,
    std::string_view description,
    std::string_view exec_name,
    std::vector<std::string> args,
    boost::asio::any_io_executor io_executor
) {
    // Construct first (launching the child), then start the I/O coroutines.
    // We cannot call shared_from_this() inside the constructor, so the
    // background tasks are started here, after make_shared has handed us a
    // owning shared_ptr.
    auto instance = std::make_shared<SubProcessInstance>(
        id,
        description,
        exec_name,
        std::move(args),
        std::move(io_executor)
    );

    instance->_start_background_tasks();
    return instance;
}

void SubProcessManager::SubProcessInstance::_start_background_tasks() {
    // Three detached coroutines, all on the instance strand. Each captures a
    // shared_ptr to `self` so the instance outlives the coroutine even if the
    // manager drops its reference. They exit naturally when their pipe closes
    // (done() closes every pipe), which lets the io_context wind down.
    boost::asio::co_spawn(_strand, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->_background_read_task(self->_pipe1, self->_buffer1);
    }, [](std::exception_ptr){});
    boost::asio::co_spawn(_strand, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->_background_read_task(self->_pipe2, self->_buffer2);
    }, [](std::exception_ptr){});
    boost::asio::co_spawn(_strand, [self = shared_from_this()]() -> boost::asio::awaitable<void> {
        co_await self->_background_write_task();
    }, [](std::exception_ptr){});
    return;
}

// Continuously read from a child output pipe into a buffer until the pipe is
// closed (EOF or done()). Because this runs on the instance strand, appends
// to `output_buffer` are race-free with respect to read()/read_delta().
boost::asio::awaitable<void> SubProcessManager::SubProcessInstance::_background_read_task(boost::asio::readable_pipe& pipe, std::string& output_buffer) noexcept {
    boost::system::error_code ec;
    std::array<char, READBUFFER_SIZE> read_buffer;
    while(true) {
        if (!pipe.is_open()) {
            break;
        }

        size_t read_size = co_await pipe.async_read_some(
            boost::asio::buffer(read_buffer, read_buffer.size()),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );

        if (read_size) {
            output_buffer.append(read_buffer.data(), read_size);
        }

        // Any error (including EOF) ends the loop; make sure the pipe is closed
        // so the write side / done() see a consistent state.
        if (ec) {
            if (pipe.is_open()) {
                pipe.close(ec);
            }
            break;
        }
    }
    co_return;
}

// Drain `_write_channel` into the child's stdin. async_receive suspends
// until write() pushes a message; no timer polling needed. The channel is
// closed by stop_writing() / done() to wake us up when the pipe shuts down.
boost::asio::awaitable<void> SubProcessManager::SubProcessInstance::_background_write_task() noexcept {
    boost::system::error_code ec;
    while (true) {
        if (!_pipe0.is_open()) {
            _write_channel.close();
            break;
        }

        // Suspend until a message arrives or the channel is closed.
        std::string message = co_await _write_channel.async_receive(
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );

        if (ec) {
            // Channel was closed (stop_writing / done / pipe error).
            break;
        }

        co_await boost::asio::async_write(
            _pipe0,
            boost::asio::buffer(message),
            boost::asio::redirect_error(boost::asio::use_awaitable, ec)
        );

        if (ec) {
            if (_pipe0.is_open()) {
                _pipe0.close(ec);
            }
            break;
        }
    }
    co_return;
}

// Reap the child: wait for it to exit, then tear down all I/O. Guarded by
// `exited` so it is safe to call multiple times (e.g. once from terminate()
// and once from the manager's background wait_done()).
//
// Cancellation-safe: spawn_and_wait() races this against a timeout. If the
// async_wait is cancelled while the child is still running, redirect_error
// captures operation_aborted into `ec` (no throw) and we then gate the reap on
// `!running()` — a still-living child is left untouched so the caller can
// detach or kill it. Only an actually-exited child is reaped.
boost::asio::awaitable<SubProcessManager::ProcessID> SubProcessManager::SubProcessInstance::done() noexcept {
    boost::system::error_code ec;
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    if (_process_ptr && _process_ptr->running()) {
        co_await _process_ptr->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    }

    // Only reap when the child has truly exited. A cancelled async_wait leaves
    // it running; reaping anyway would drop the handle and orphan it.
    if (!exited && _process_ptr && !_process_ptr->running()) {
        exited = true;
        end_point = Clock::now();
        exit_code = _process_ptr->exit_code();
        // Close the write channel first to wake the background write task,
        // then close all pipes to signal EOF to the read tasks.
        _write_channel.close();
        if (_pipe0.is_open()) {
            _pipe0.close(ec);
        }
        if (_pipe1.is_open()) {
            _pipe1.close(ec);
        }
        if (_pipe2.is_open()) {
            _pipe2.close(ec);
        }
        _process_ptr.reset();
    }
    co_return id;
}

// Forceful shutdown: SIGTERM the child, then go through done() to reap and
// tear down I/O. The `running()` guard avoids sending a signal to an
// already-dead process.
boost::asio::awaitable<SubProcessManager::ProcessID> SubProcessManager::SubProcessInstance::terminate() noexcept {
    boost::system::error_code ec;
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    if (_process_ptr && _process_ptr->running()) {
        _process_ptr->terminate(ec);
    }
    auto id = co_await done();
    co_return id;
}

// Push data to the background write task via the channel. The channel wakes
// the write task immediately — no explicit wakeup signal needed.
boost::asio::awaitable<bool> SubProcessManager::SubProcessInstance::write(std::string_view message) noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    if (!_pipe0.is_open() || !_write_channel.is_open()) {
        co_return false;
    }

    boost::system::error_code ec;
    co_await _write_channel.async_send(
        boost::system::error_code{},    // transport status (success)
        std::string(message),           // payload
        boost::asio::redirect_error(boost::asio::use_awaitable, ec)
    );
    co_return !ec;
}

// Snapshot the *full* accumulated output for a stream. STDIN is not readable,
// so it reports nullopt.
boost::asio::awaitable<std::optional<std::string>> SubProcessManager::SubProcessInstance::read(IO stdio) noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    if (stdio == IO::STDIN) {
        co_return std::nullopt;
    } else if (stdio == IO::STDOUT) {
        co_return _buffer1;
    } else if (stdio == IO::STDERR) {
        co_return _buffer2;
    }
    co_return std::nullopt;
}

// Return only what has been appended since the last read_delta() on this
// stream, advancing the per-buffer cursor. nullopt means "nothing new" (or
// STDIN). This is what collect_*() uses by default so callers can poll
// incrementally without re-receiving old output.
boost::asio::awaitable<std::optional<std::string>> SubProcessManager::SubProcessInstance::read_delta(IO stdio) noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    if (stdio == IO::STDIN) {
        co_return std::nullopt;
    } else if (stdio == IO::STDOUT) {
        if (_cursor1 < _buffer1.size()) {
            auto result = _buffer1.substr(_cursor1);
            _cursor1 = _buffer1.size();
            co_return result;
        } else {
            co_return std::nullopt;
        }
    } else if (stdio == IO::STDERR) {
        if (_cursor2 < _buffer2.size()) {
            auto result = _buffer2.substr(_cursor2);
            _cursor2 = _buffer2.size();
            co_return result;
        } else {
            co_return std::nullopt;
        }
    }
    co_return std::nullopt;
}

// Half-close stdin so the child observes EOF. Closes the write channel first
// to wake the background write task, then closes the pipe. After this,
// write() returns false.
boost::asio::awaitable<void> SubProcessManager::SubProcessInstance::stop_writing() noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    boost::system::error_code ec;
    _write_channel.close();
    if (_pipe0.is_open()) {
        _pipe0.close(ec);
    }
    co_return;
}

// JSON status snapshot. Once `exited`, the timing is fixed (end_point). While
// still alive we report "running" / "finished" (process handle present but
// not yet reaped) / "unknown" (no handle, e.g. constructed but not launched).
boost::asio::awaitable<nlohmann::json> SubProcessManager::SubProcessInstance::execution_status() const noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    if (exited) {
        auto execution_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(end_point - start_point).count();
        auto result = nlohmann::json({
            {"status", "exited"},
            {"exit_code", exit_code},
            {"execution_milliseconds", execution_milliseconds}
        });
        co_return result;
    } else {
        auto accum_point = Clock::now();
        auto execution_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(accum_point - start_point).count();
        std::string status = "unknown";
        if (_process_ptr) {
            if (_process_ptr->running()) {
                status = "running";
            } else {
                status = "finished";
            }
        }
        auto result = nlohmann::json({
            {"status", status},
            {"exit_code", nullptr},
            {"execution_milliseconds", execution_milliseconds}
        });
        co_return result;
    }
}

// ============================================================================
// SubProcessManager
// ============================================================================

SubProcessManager::SubProcessManager(boost::asio::any_io_executor io_executor):
_top_id(0), _strand(std::move(io_executor)), _instances(), _free(), _running(), _finished() {}

SubProcessManager::~SubProcessManager() {}

// Hand out a (possibly recycled) ID, launch the instance on the manager's
// executor, and register it as running. When @p autoreap, also start a
// background reaper that moves it to `_finished` as soon as the child exits.
// spawn_and_wait() passes autoreap=false and reaps inline so it can reclaim
// the id itself without racing a detached reaper.
boost::asio::awaitable<SubProcessManager::ProcessID> SubProcessManager::_spawn(
    std::string_view description,
    std::string_view exec_name,
    std::vector<std::string> args,
    bool autoreap
) {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);

    ProcessID current_id;
    if (_free.empty()) {
        current_id = _top_id++;
    } else {
        auto it = _free.begin();
        current_id = *it;
        _free.erase(it);
    }

    _instances[current_id] = SubProcessInstance::create(current_id, description, exec_name, std::move(args), _strand.get_inner_executor());
    _running.insert(current_id);

    if (autoreap) {
        // Detached reaper: keeps the manager alive (shared_from_this) until the
        // child is reaped, then transitions _running → _finished.
        boost::asio::co_spawn(_strand, [current_id, self = shared_from_this()]() -> boost::asio::awaitable<void> {
            co_await self->wait_done(current_id);
        }, [](std::exception_ptr){});
    }
    co_return current_id;
}

boost::asio::awaitable<SubProcessManager::ProcessID> SubProcessManager::spawn(
    std::string_view description,
    std::string_view exec_name,
    std::vector<std::string> args
) {
    co_return co_await _spawn(description, exec_name, std::move(args), /*autoreap=*/true);
}

// spawn_and_wait owns the wait itself: it spawns WITHOUT a background reaper
// (autoreap=false), so its done() holds the single async_wait on the child,
// then races that against a timeout. There is no concurrent reaper, so when it
// reclaims the id for a reaped child nothing else is mid-bookkeeping for that
// id. awaitable_operators' operator|| cancels the losing branch and waits for
// it to unwind before returning, so a cancelled done() has fully released the
// async_wait before we act on the timeout.
boost::asio::awaitable<nlohmann::json> SubProcessManager::spawn_and_wait(
    std::string_view description,
    std::string_view exec_name,
    std::vector<std::string> args,
    Clock::duration timeout,
    bool kill
) {
    auto id = co_await _spawn(description, exec_name, std::move(args), /*autoreap=*/false);
    auto instance = co_await get(id);
    if (!instance) {
        // Spawn reported success but the instance vanished before get() — emit a
        // minimal ProcessReport-shaped unknown status so callers still see the
        // contract's meta fields.
        nlohmann::json unknown_status = {
            {"status", "unknown"},
            {"exit_code", nullptr},
            {"execution_milliseconds", 0}
        };
        co_return schema::ProcessReport(id, description, unknown_status).build();
    }

    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, timeout);
    boost::system::error_code timer_ec;
    using boost::asio::experimental::awaitable_operators::operator||;
    // Race reap-vs-timeout. Both branches use redirect_error so the cancelled
    // loser completes cleanly (no throw). winner.index() 0 = done() reaped
    // (child exited), 1 = timer fired first.
    auto winner = co_await (
        instance->done() ||
        timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, timer_ec))
    );

    if (winner.index() == 1) {
        // Timer won → timeout. done() was cancelled; the child is (almost
        // always) still running. SIGTERM+reap when asked, otherwise leave it
        // running (detach).
        if (kill) {
            co_await instance->terminate();
        }
    }
    // winner.index() == 0: done() reaped the child on natural exit.

    // Build the full ProcessReport: meta from execution_status(), plus the
    // accumulated stdout/stderr (complete when reaped, partial when detached).
    auto status = co_await instance->execution_status();
    auto out = co_await instance->read(IO::STDOUT);
    auto err = co_await instance->read(IO::STDERR);
    auto report = schema::ProcessReport(instance->id, instance->description, status)
                      .stream(out, err)
                      .build();

    // Reclaim the id for a reaped child. Done as ONE dispatch on the manager
    // strand with no suspension between the erase/free, so a concurrent
    // spawn() cannot recycle this id mid-removal. A detached child is left in
    // _instances/_running and handed to a freshly started reaper instead.
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    if (status["status"] == "exited") {
        _instances.erase(id);
        _running.erase(id);
        _finished.erase(id);
        _free.insert(id);
    } else {
        // Detached: start the background reaper so the still-living child is
        // eventually reaped and a later collect_*() can observe it.
        boost::asio::co_spawn(_strand, [id, self = shared_from_this()]() -> boost::asio::awaitable<void> {
            co_await self->wait_done(id);
        }, [](std::exception_ptr){});
    }
    co_return report;
}

// Background reaper for a single process. No-op if the id isn't running (e.g.
// already collected). If the instance vanished, just recycle its id.
boost::asio::awaitable<void> SubProcessManager::wait_done(ProcessID id) noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    if (_running.count(id) == 0) {
        co_return;
    }

    auto it = _instances.find(id);
    if (it == _instances.end() || it->second == nullptr) {
        _running.erase(id);
        _free.insert(id);
        co_return;
    }

    auto instance = it->second; // not nullptr
    co_await instance->done();

    // done() is idempotent, so this is safe even if terminate() already reaped
    // it; the re-check of _running guards against double-transition if the id
    // was collected concurrently.
    if (_running.count(id)) {
        _running.erase(id);
        _finished.insert(id);
    }
    co_return;
}

boost::asio::awaitable<std::shared_ptr<SubProcessManager::SubProcessInstance>> SubProcessManager::get(ProcessID id) const noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    auto it = _instances.find(id);
    if (it == _instances.end()) {
        co_return nullptr;
    } else {
        co_return it->second;
    }
}

// Snapshot every live instance. We copy the _instances map before iterating
// so the per-instance execution_status() coroutines (which re-dispatch on the
// strand) don't invalidate our iterator.
boost::asio::awaitable<nlohmann::json> SubProcessManager::list_status() const noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    nlohmann::json result = nlohmann::json::array();

    auto instances = _instances;
    for (const auto& [id, instance]: instances) {
        if (instance == nullptr) {
            continue;
        }

        // Meta-only ProcessReport (no output streams) — see schema.hpp.
        auto status = co_await instance->execution_status();
        result.push_back(
            schema::ProcessReport(instance->id, instance->description, status).build());
    }
    co_return result;
}

// Remove every reaped instance from the manager, report its final status +
// output, and recycle its id. Instances are moved into a local vector first
// so the bookkeeping (_instances.erase / _free.insert / _finished.clear)
// completes before we await on the instances.
boost::asio::awaitable<nlohmann::json> SubProcessManager::collect_finished(bool full_output) noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    nlohmann::json result = nlohmann::json::array();

    std::vector<std::shared_ptr<SubProcessInstance>> instances;
    for (auto id: _finished) {
        auto it = _instances.find(id);
        if (it != _instances.end() && it->second != nullptr) {
            instances.push_back(std::move(it->second));
        }
        _instances.erase(it);
        _free.insert(id);
    }
    _finished.clear();

    for (auto& instance: instances) {
        // ProcessReport with both output streams — full buffers when
        // full_output, otherwise only the delta since the last collect_*
        // (nullopt serialises to null). See schema.hpp.
        auto status = co_await instance->execution_status();
        auto out = full_output ? co_await instance->read(IO::STDOUT)
                               : co_await instance->read_delta(IO::STDOUT);
        auto err = full_output ? co_await instance->read(IO::STDERR)
                               : co_await instance->read_delta(IO::STDERR);
        result.push_back(
            schema::ProcessReport(instance->id, instance->description, status)
                .stream(out, err)
                .build());
    }

    co_return result;
}

// Snapshot running instances WITHOUT removing them (the caller may still want
// to interact with them). Mirrors collect_finished's output shape.
boost::asio::awaitable<nlohmann::json> SubProcessManager::collect_running(bool full_output) const noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);
    nlohmann::json result = nlohmann::json::array();

    std::vector<std::shared_ptr<SubProcessInstance>> instances;
    for (auto id: _running) {
        auto it = _instances.find(id);
        if (it != _instances.end() && it->second != nullptr) {
            instances.push_back(it->second);
        }
    }

    for (auto& instance: instances) {
        // ProcessReport with both output streams — full buffers when
        // full_output, otherwise only the delta since the last collect_*
        // (nullopt serialises to null). See schema.hpp.
        auto status = co_await instance->execution_status();
        auto out = full_output ? co_await instance->read(IO::STDOUT)
                               : co_await instance->read_delta(IO::STDOUT);
        auto err = full_output ? co_await instance->read(IO::STDERR)
                               : co_await instance->read_delta(IO::STDERR);
        result.push_back(
            schema::ProcessReport(instance->id, instance->description, status)
                .stream(out, err)
                .build());
    }

    co_return result;
}

// Shutdown helper: SIGTERM every still-running instance and reap it. The
// background wait_done reapers will then move them to `_finished`, where a
// subsequent collect_finished() can drain them.
boost::asio::awaitable<void> SubProcessManager::terminate_all() noexcept {
    co_await boost::asio::dispatch(_strand, boost::asio::use_awaitable);

    std::vector<std::shared_ptr<SubProcessInstance>> instances;
    for (auto id: _running) {
        auto it = _instances.find(id);
        if (it != _instances.end() && it->second != nullptr) {
            instances.push_back(it->second);
        }
    }

    for (auto& instance: instances) {
        co_await instance->terminate();
    }

    co_return;
}

}

