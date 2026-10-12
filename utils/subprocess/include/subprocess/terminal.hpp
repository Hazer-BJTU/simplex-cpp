#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include "subprocess/child.hpp"

namespace subprocess {

/** Linux PTY mechanism, serialized by its host executor. Owns a master and a
 * temporary launch slave; all native descriptors and terminal settings remain
 * private. Construction on other platforms fails explicitly. Destruction is
 * not a task fence: callers must close and join outstanding operations first.
 */
class Terminal {
public:
    /// Allocate a CLOEXEC master/slave pair and set deterministic child defaults.
    /// Allocation/configuration is synchronous and leaves host terminal state alone.
    explicit Terminal(boost::asio::any_io_executor executor);
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    /// Borrow once for Child construction. The parent slave closes on either
    /// launch success or failure; retaining the attachment cannot hold it open.
    IoAttachment attachment();
    /// True only after Child's launch succeeded; allocation alone is not launch.
    [[nodiscard]] bool launched() const noexcept;
    /// Read raw merged bytes. Linux EIO becomes EOF only after successful launch
    /// and a confirmed slave hangup; device/live-slave errors remain failures.
    boost::asio::awaitable<std::size_t>
    read(boost::asio::mutable_buffer buffer, boost::system::error_code& error);
    /// Write the complete input or return a partial byte count with error.
    /// Keep the borrowed string alive until completion and serialize writes.
    boost::asio::awaitable<std::size_t>
    write(std::string_view bytes, boost::system::error_code& error);
    /// Read the CURRENT canonical/EOF settings. Two EOF characters flush an
    /// unterminated canonical line then request an empty read. This is neither
    /// a physical half-close nor a promise that a program consumes/exits on EOF.
    /// A concurrent child termios change can still invalidate these semantics.
    [[nodiscard]] std::optional<std::string> eof_request();
    /// Apply nonzero dimensions in [1, 65535]; OS errors propagate to the host.
    void resize(unsigned columns, unsigned rows);
    /// Cancels writes only; the independently owned read side remains open.
    void cancel_input(boost::system::error_code& error) noexcept;
    /// Close both master owners and any launch slave, preserving the first error.
    /// Final master closure may hang up slave holders; it is not process-tree kill.
    void close(boost::system::error_code& error) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    friend class Child;
    /// Release the parent's slave on every launch outcome, including validation
    /// failure before fork, so it cannot keep the terminal output alive.
    void finish_launch(bool success) noexcept;
    int launch_descriptor() const; // private platform boundary, never public
};

} // namespace subprocess
