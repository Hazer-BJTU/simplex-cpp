#pragma once

#include <chrono>
#include <exception>
#include <string>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>

#include "process_io/capture.hpp"
#include "subprocess/child.hpp"

namespace process_io {

using Executor = boost::asio::strand<boost::asio::any_io_executor>;
enum class InputEndMode { Unsupported, HalfClose, TerminalEof };
enum class InputPhase { NotStarted, Open, EndRequested, Ended, PeerClosed, Stopped, Failed };
enum class InputResult { Accepted, Closed, Unsupported };
enum class ResizeResult { Applied, Unsupported };

struct Capabilities {
    OutputTopology topology;
    bool is_terminal;
    InputEndMode input_end;
    bool can_resize;
};

/** A borrowed observation; inspect only on executor(), without suspension.
 * Stream absence, empty content, EOF, forced stop and read failure are distinct.
 * output_complete means capture ended, not lossless output or joined tasks.
 * joined is the explicit cancel_and_join() fence, not a child-exit observation.
 * failure retains the first unexpected I/O/task error, also rethrown after join.
 */
struct Snapshot {
    std::optional<StreamView> standard_output;
    std::optional<StreamView> standard_error;
    std::optional<StreamView> terminal_output;
    InputPhase input_phase;
    bool accepting_input;
    bool input_end_requested;
    bool output_complete;
    bool joined;
    std::exception_ptr failure;
};

/** Independent child I/O, with no process ID, reaper, timeout or tool policy.
 * Construct with the SAME strand used by child lifecycle/session observations.
 * Awaitable operations marshal onto that strand and can be called from other
 * executors. prepare_attachment() and snapshot() must run directly on it;
 * snapshot/cursor consumption must be one operation without co_await in between.
 * Keep the environment alive throughout outstanding method calls. The executor
 * context must outlive it. Explicit cancel_and_join() is the resource fence.
 */
class ProcessIoEnvironment {
public:
    virtual ~ProcessIoEnvironment() = default;
    [[nodiscard]] virtual Executor executor() const = 0;
    [[nodiscard]] virtual Capabilities capabilities() const noexcept = 0;

    /// Once only, before launching the child. The attachment borrows this
    /// environment until launch completes. A failed launch must still be
    /// followed by cancel_and_join(); that path needs no child or start().
    virtual subprocess::IoAttachment prepare_attachment() = 0;
    /// After successful launch. Supervises all pumps; repeated start is an error.
    virtual boost::asio::awaitable<void> start() = 0;
    /// Ordered queue acceptance, NOT delivery or child consumption. No strict
    /// pending-input memory quota is promised; the caller controls input volume.
    virtual boost::asio::awaitable<InputResult> enqueue_input(std::string input) = 0;
    /// Sequence an input-end request after all accepted input. Accepted does
    /// not promise delivery, process exit, or universal terminal half-close.
    virtual boost::asio::awaitable<InputResult> request_input_end() = 0;
    /// Reject later input without closing output. Already accepted input can
    /// still be delivered; this does not itself request EOF or join anything.
    virtual boost::asio::awaitable<void> stop_admission() = 0;
    [[nodiscard]] virtual Snapshot snapshot() const = 0;
    /// Permit natural capture until the deadline. False means time expired;
    /// does not close transports. Caller cancellation can interrupt this wait.
    virtual boost::asio::awaitable<bool> drain_until(
        std::chrono::steady_clock::time_point deadline) = 0;
    /// Shield cancellation, stop admission, discard undelivered input, close
    /// transports, join EVERY owned pump, then rethrow the first failure.
    /// Safe before start/after launch failure, and repeatable even after errors.
    virtual boost::asio::awaitable<void> cancel_and_join() = 0;
    /// Optional host-only capability; no native dimensions/descriptors escape.
    virtual boost::asio::awaitable<ResizeResult> resize(
        unsigned columns, unsigned rows) = 0;
};

} // namespace process_io
