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
    explicit Terminal(boost::asio::any_io_executor executor);
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    /// Borrow once for Child construction. The parent slave closes on either
    /// launch success or failure; retaining the attachment cannot hold it open.
    IoAttachment attachment();
    [[nodiscard]] bool launched() const noexcept;
    boost::asio::awaitable<std::size_t> read(
        boost::asio::mutable_buffer buffer, boost::system::error_code& error);
    boost::asio::awaitable<std::size_t> write(
        std::string_view bytes, boost::system::error_code& error);
    /// Read the CURRENT canonical/EOF settings. Two EOF characters flush an
    /// unterminated canonical line then request an empty read. This is neither
    /// a physical half-close nor a promise that a program consumes/exits on EOF.
    /// A concurrent child termios change can still invalidate these semantics.
    [[nodiscard]] std::optional<std::string> eof_request();
    void resize(unsigned columns, unsigned rows);
    /// Cancels writes only; the independently owned read side remains open.
    void cancel_input(boost::system::error_code& error) noexcept;
    void close(boost::system::error_code& error) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    friend class Child;
    void finish_launch(bool success) noexcept;
    int launch_descriptor() const; // private platform boundary, never public
};

} // namespace subprocess
