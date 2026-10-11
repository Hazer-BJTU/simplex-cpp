#pragma once

#include <memory>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/system/error_code.hpp>

#include "dataclass/process_spec.hpp"
#include "subprocess/exception.hpp"

namespace subprocess {

/**
 * Opaque launch attachment. It borrows the supplied transport objects until
 * Child construction finishes; the caller must keep them alive throughout
 * launch. Native descriptors and platform launcher initializers stay private.
 * Future terminal backends can supply a different private representation.
 */
class IoAttachment {
public:
    static IoAttachment pipes(
        boost::asio::writable_pipe& input,
        boost::asio::readable_pipe& output,
        boost::asio::readable_pipe& error);
    ~IoAttachment();
    IoAttachment(IoAttachment&&) noexcept;
    IoAttachment& operator=(IoAttachment&&) noexcept;
    IoAttachment(const IoAttachment&) = delete;
    IoAttachment& operator=(const IoAttachment&) = delete;

private:
    struct Impl;
    explicit IoAttachment(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> _impl;
    friend class Child;
    friend class Terminal;
};

/**
 * Owned child-process mechanism. Contains the native process object, resolves
 * the executable and environment, and performs launch, signal and reap calls.
 * It has no deadline, session, capture or scheduling policy. The owner must
 * serialize operations on one executor and retain this object during wait().
 * Construction failures throw ProcessException with the original launch stage.
 */
class Child {
public:
    Child(const process::LaunchSpec& spec,
          boost::asio::any_io_executor executor,
          const IoAttachment& attachment);
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    /// Diagnostic identifier, not a native resource handle.
    [[nodiscard]] process::ProcessId id() const noexcept;
    /// Probe and reap if finished; false with an error is not successful exit.
    bool running(boost::system::error_code& error);
    /// Wait on the owned process; cancellation follows the caller's coroutine.
    boost::asio::awaitable<int> wait(boost::system::error_code& error);
    /// Hard terminate and reap, using the platform process implementation.
    void terminate(boost::system::error_code& error);
    /// Request graceful direct-child exit without waiting for completion.
    void request_exit(boost::system::error_code& error);
    /// Valid only after successful terminal observation/reaping.
    [[nodiscard]] int exit_code() const;

    /**
     * Best-effort synchronous kill/reap for abandoned owners. No operations may
     * run concurrently: stop the executor and join its workers first. This is
     * not an asynchronous task-join fence. Errors are returned, never thrown.
     * Repeated calls use the owned child's reaped state, never a cached PID.
     */
    boost::system::error_code emergency_cleanup() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace subprocess
