/**
 * @file subprocess.hpp
 * @brief Asynchronous subprocess management for the indextools framework.
 *
 * This header provides SubProcessManager, a small async process supervisor
 * built on top of Boost.Asio coroutines and Boost.Process (v2). It allows the
 * tool server to spawn long-running child processes, feed their stdin, stream
 * their stdout/stderr, inspect their status, and terminate them — all without
 * blocking the WebSocket I/O thread.
 *
 * # Threading model
 *
 * Every SubProcessManager owns a single boost::asio::strand. All state
 * mutations (the `_instances` / `_running` / `_finished` / `_free` containers
 * and the per-instance fields) happen on that strand: each public coroutine
 * begins with `co_await dispatch(_strand, ...)` so the body runs serialized.
 * Callers may therefore invoke the manager from any thread / coroutine without
 * external locking — the strand is the synchronisation primitive.
 *
 * Each SubProcessInstance additionally owns its *own* strand (a child of the
 * manager's executor) so that per-process I/O (pipe reads/writes, status
 * queries) is serialised independently and does not contend across processes.
 *
 * # Lifetime
 *
 * Both classes inherit std::enable_shared_from_this and are meant to be owned
 * via std::shared_ptr. Background coroutines (the per-instance read/write
 * tasks and the manager's `wait_done` reaper) capture a `shared_from_this()`,
 * keeping the object alive until the coroutine completes — even if the caller
 * drops its handle. A process is fully reclaimed only after `done()` has run,
 * which closes all three pipes and resets the process handle.
 *
 * # ID allocation
 *
 * Process IDs are uint64_t tokens handed out by spawn(). Freed IDs (after
 * collect_finished()) are recycled via the `_free` pool before minting new
 * ones from the monotonic `_top_id` counter, so the ID space stays compact.
 */

#pragma once

#include <cstdlib>
#include <iostream>
#include <format>
#include <string>
#include <sstream>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_set>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/process.hpp>
#include <boost/system.hpp>

#include <nlohmann/json.hpp>

namespace indextools {

/**
 * @brief Async supervisor that owns a pool of child processes.
 *
 * The manager hands out ProcessIDs, tracks which processes are still running
 * vs. finished, reaps exited processes in the background, and exposes
 * collection/termination helpers that return JSON status snapshots.
 *
 * All public methods are coroutines (`boost::asio::awaitable<...>`) that
 * marshal onto the manager's strand, so they are safe to call concurrently.
 */
class SubProcessManager: public std::enable_shared_from_this<SubProcessManager> {
public:
    using ProcessID = uint64_t;                 ///< Opaque handle returned by spawn().
    using Clock = std::chrono::steady_clock;    ///< Clock used for execution timing.

    /// Selects which standard stream a read operation targets.
    enum class IO { STDIN, STDOUT, STDERR };

//private:
    /**
     * @brief A single managed child process and its async I/O plumbing.
     *
     * An instance is always created through create() (which launches the
     * process and starts the background I/O coroutines) and held by the
     * manager via shared_ptr. It is non-copyable and non-movable because it
     * owns asio objects (pipes, a timer, a strand) that are tied to a fixed
     * executor and address.
     *
     * Data flow:
     *   - stdout/stderr are continuously drained by two background read
     *     tasks into `_buffer1` / `_buffer2`; callers snapshot them via
     *     read() (whole buffer) or read_delta() (bytes since last call).
     *   - stdin is fed by a background write task that drains `_write_queue`;
     *     callers enqueue data with write() and the task flushes it
     *     asynchronously.
     */
    struct SubProcessInstance: std::enable_shared_from_this<SubProcessInstance> {
        /// Per-read buffer size for the background stdout/stderr drainers.
        static constexpr size_t READBUFFER_SIZE = 4096;

        const ProcessID id;                 ///< Unique handle assigned by the manager.
        const std::string description;      ///< Human-readable label for status reports.
        const Clock::time_point start_point;///< Spawn timestamp (for execution timing).

        bool exited;                        ///< True once done() has reaped the process.
        Clock::time_point end_point;        ///< When the process was reaped.
        std::optional<int> exit_code;       ///< Exit code, set when exited (signal death reflected per OS).

        boost::asio::strand<boost::asio::any_io_executor> _strand;  ///< Serialises all instance state.
        std::unique_ptr<boost::process::process> _process_ptr;      ///< The Boost.Process child handle.
        boost::asio::writable_pipe _pipe0;       ///< Child's stdin  (we write, child reads).
        boost::asio::readable_pipe _pipe1, _pipe2; ///< Child's stdout/stderr (we read).
        /// Channel that replaces the queue+idle-timer pair: write() pushes
        /// messages via async_send; the background write task drains them via
        /// async_receive, suspending until data arrives (no polling).
        boost::asio::experimental::channel<void(boost::system::error_code, std::string)> _write_channel;
        std::string _buffer1, _buffer2;          ///< Accumulated stdout / stderr output.
        size_t _cursor1, _cursor2;               ///< Read cursors for read_delta() on each buffer.

        SubProcessInstance(
            ProcessID id,
            std::string_view description,
            std::string_view exec_name,
            std::vector<std::string> args,
            boost::asio::any_io_executor io_executor
        );
        ~SubProcessInstance();
        SubProcessInstance(const SubProcessInstance&) = delete;
        SubProcessInstance& operator = (const SubProcessInstance&) = delete;
        SubProcessInstance(SubProcessInstance&&) = delete;
        SubProcessInstance& operator = (SubProcessInstance&&) = delete;

        /// Factory: launches the process and starts the background I/O tasks.
        static std::shared_ptr<SubProcessInstance> create(
            ProcessID id,
            std::string_view description,
            std::string_view exec_name,
            std::vector<std::string> args,
            boost::asio::any_io_executor io_executor
        );

        /// Spawns the three background coroutines (stdout/stderr readers, stdin writer).
        void _start_background_tasks();
        /// Background loop: append everything readable from @p pipe into @p output_buffer until EOF.
        boost::asio::awaitable<void> _background_read_task(boost::asio::readable_pipe& pipe, std::string& output_buffer) noexcept;
        /// Background loop: drain `_write_channel` into the child's stdin, suspending when empty.
        boost::asio::awaitable<void> _background_write_task() noexcept;

        /// Block until the process exits, then close all pipes and record exit_code. Idempotent.
        boost::asio::awaitable<ProcessID> done() noexcept;
        /// Send SIGTERM, then done(). Idempotent.
        boost::asio::awaitable<ProcessID> terminate() noexcept;
        /// Enqueue @p message to the child's stdin. Returns false if stdin is already closed.
        boost::asio::awaitable<bool> write(std::string_view message) noexcept;
        /// Return the full accumulated buffer for the chosen stream (nullopt for STDIN).
        boost::asio::awaitable<std::optional<std::string>> read(IO stdio) noexcept;
        /// Return only the bytes appended since the last read_delta() on @p stdio (nullopt if none / STDIN).
        boost::asio::awaitable<std::optional<std::string>> read_delta(IO stdio) noexcept;
        /// Close the child's stdin (sends EOF). Subsequent write() calls return false.
        boost::asio::awaitable<void> stop_writing() noexcept;
        /// JSON snapshot: status ("running"/"finished"/"exited"), exit_code, execution_milliseconds.
        boost::asio::awaitable<nlohmann::json> execution_status() const noexcept;
    };

private:
    ProcessID _top_id;        ///< Monotonic counter for minting fresh IDs.
    boost::asio::strand<boost::asio::any_io_executor> _strand;  ///< Serialises all manager state.
    std::unordered_map<ProcessID, std::shared_ptr<SubProcessInstance>> _instances;  ///< Every live instance by ID.
    std::unordered_set<ProcessID> _free, _running, _finished;  ///< ID pools: recyclable / unreaped / reaped.

public:
    SubProcessManager(boost::asio::any_io_executor io_executor);
    ~SubProcessManager();
    SubProcessManager(const SubProcessManager&) = delete;
    SubProcessManager& operator = (const SubProcessManager&) = delete;
    SubProcessManager(SubProcessManager&&) = delete;
    SubProcessManager& operator = (SubProcessManager&&) = delete;

    /**
     * @brief Launch a new child process.
     *
     * @p exec_name is resolved through PATH (via
     * boost::process::environment::find_executable) and spawned with @p args
     * and the current environment. A background reaper (wait_done) is started
     * so the process is moved to `_finished` automatically once it exits.
     *
     * @return The new ProcessID (recycled from `_free` if available).
     */
    boost::asio::awaitable<ProcessID> spawn(
        std::string_view description,
        std::string_view exec_name,
        std::vector<std::string> args
    );

    /// Block until @p id has exited and been reaped. No-op if unknown / already reaped.
    boost::asio::awaitable<void> wait_done(ProcessID id) noexcept;

    /// Look up the instance for @p id (nullptr if unknown). Safe to call while reaping.
    boost::asio::awaitable<std::shared_ptr<SubProcessInstance>> get(ProcessID id) const noexcept;

    /// JSON array of {id, description, execution_status} for every live instance.
    boost::asio::awaitable<nlohmann::json> list_status() const noexcept;

    /**
     * @brief Drain and remove all reaped (finished) instances.
     *
     * For each finished instance: build a {id, description, execution_status,
     * stdout, stderr} entry. When @p full_output is true, stdout/stderr hold
     * the complete buffers; otherwise only the deltas since the last
     * collect_*() call are reported. Reaped instances are removed from the
     * manager and their IDs returned to `_free`.
     */
    boost::asio::awaitable<nlohmann::json> collect_finished(bool full_output = false) noexcept;

    /// Like collect_finished(), but snapshots currently-*running* instances without removing them.
    boost::asio::awaitable<nlohmann::json> collect_running(bool full_output = false) const noexcept;

    /// SIGTERM every still-running instance and reap it. Used for shutdown.
    boost::asio::awaitable<void> terminate_all() noexcept;
};

}
