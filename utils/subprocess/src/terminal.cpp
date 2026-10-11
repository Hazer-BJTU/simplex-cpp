#include "subprocess/terminal.hpp"
#include "attachment.hpp"
#include "native_fd.hpp"
#include <limits>
#include <stdexcept>
#include <utility>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#ifdef __linux__
#include <boost/asio/posix/stream_descriptor.hpp>
#include <cstdlib>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#endif

namespace subprocess {
namespace asio = boost::asio;
#ifdef __linux__
struct Terminal::Impl {
    asio::posix::stream_descriptor reader;
    asio::posix::stream_descriptor writer;
    detail::Fd slave;
    bool attached = false;
    bool launched = false;

    explicit Impl(asio::any_io_executor executor) : reader(executor), writer(executor)
    {
        detail::Fd master(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK));
        detail::move_above_stdio(master);
        detail::check(::grantpt(master.get()), "grantpt");
        detail::check(::unlockpt(master.get()), "unlockpt");
        // TIOCGPTPEER opens this master's exact slave without pathname lookup
        // races or ptsname's shared buffer. Linux >= 4.13 is required.
        slave.reset(::ioctl(master.get(), TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC));
        detail::move_above_stdio(slave);
        termios settings{};
        detail::check(::tcgetattr(slave.get(), &settings), "tcgetattr(initial)");
        settings.c_iflag = ICRNL | IXON;
        settings.c_oflag = OPOST | ONLCR;
        settings.c_cflag = CS8 | CREAD | CLOCAL;
        settings.c_lflag = ICANON | ISIG | IEXTEN; // deliberately no ECHO/ECHONL
        for (auto& value : settings.c_cc) value = _POSIX_VDISABLE;
        settings.c_cc[VINTR] = 3;
        settings.c_cc[VQUIT] = 28;
        settings.c_cc[VERASE] = 127;
        settings.c_cc[VKILL] = 21;
        settings.c_cc[VEOF] = 4;
        settings.c_cc[VSTART] = 17;
        settings.c_cc[VSTOP] = 19;
        settings.c_cc[VSUSP] = 26;
        settings.c_cc[VMIN] = 1;
        detail::check(::cfsetispeed(&settings, B38400), "cfsetispeed");
        detail::check(::cfsetospeed(&settings, B38400), "cfsetospeed");
        detail::check(::tcsetattr(slave.get(), TCSANOW, &settings), "tcsetattr(initial)");
        winsize size{};
        size.ws_col = 80;
        size.ws_row = 24;
        detail::check(::ioctl(slave.get(), TIOCSWINSZ, &size), "TIOCSWINSZ(initial)");
        detail::Fd copy(::fcntl(master.get(), F_DUPFD_CLOEXEC, 3));
        detail::check(copy.get(), "duplicate terminal master");
        reader.assign(master.get());
        master.release();
        writer.assign(copy.get());
        copy.release();
    }

    bool hung_up() noexcept
    {
        if (!reader.is_open()) return false;
        pollfd fd{reader.native_handle(), POLLIN, 0};
        return ::poll(&fd, 1, 0) > 0 && (fd.revents & POLLHUP);
    }
};
#else
struct Terminal::Impl {};
#endif

Terminal::Terminal(asio::any_io_executor executor)
{
#ifdef __linux__
    _impl = std::make_unique<Impl>(std::move(executor));
#else
    (void)executor;
    throw std::runtime_error("POSIX PTY backend is supported on Linux only");
#endif
}
Terminal::~Terminal() = default;
IoAttachment Terminal::attachment()
{
#ifdef __linux__
    if (_impl->attached) throw std::logic_error("terminal attachment already prepared");
    _impl->attached = true;
    auto impl = std::make_unique<IoAttachment::Impl>();
    impl->terminal = this;
    return IoAttachment(std::move(impl));
#else
    throw std::runtime_error("POSIX PTY backend is supported on Linux only");
#endif
}
bool Terminal::launched() const noexcept
{
#ifdef __linux__
    return _impl->launched;
#else
    return false;
#endif
}
int Terminal::launch_descriptor() const
{
#ifdef __linux__
    if (_impl->slave.get() < 0) throw std::logic_error("terminal launch already attempted");
    return _impl->slave.get();
#else
    throw std::runtime_error("POSIX PTY backend is supported on Linux only");
#endif
}
void Terminal::finish_launch(bool success) noexcept
{
#ifdef __linux__
    _impl->slave.reset();
    if (success) _impl->launched = true;
#else
    (void)success;
#endif
}
asio::awaitable<std::size_t> Terminal::read(asio::mutable_buffer buffer,
    boost::system::error_code& error)
{
#ifdef __linux__
    const auto count = co_await _impl->reader.async_read_some(buffer,
        asio::redirect_error(asio::use_awaitable, error));
    // Linux master EIO with a confirmed slave hangup is EOF. An injected or
    // device EIO while the slave is still attached remains a real failure.
    if (error.value() == EIO && _impl->launched && _impl->hung_up()) {
        error = asio::error::eof;
    }
    co_return count;
#else
    (void)buffer;
    error = asio::error::operation_not_supported;
    co_return 0;
#endif
}
asio::awaitable<std::size_t> Terminal::write(std::string_view bytes,
    boost::system::error_code& error)
{
#ifdef __linux__
    const auto count = co_await asio::async_write(_impl->writer, asio::buffer(bytes),
        asio::redirect_error(asio::use_awaitable, error));
    if (error.value() == EIO && _impl->launched && _impl->hung_up()) {
        error = asio::error::eof;
    }
    co_return count;
#else
    (void)bytes;
    error = asio::error::operation_not_supported;
    co_return 0;
#endif
}
std::optional<std::string> Terminal::eof_request()
{
#ifdef __linux__
    termios settings{};
    detail::check(::tcgetattr(_impl->reader.native_handle(), &settings), "tcgetattr(input end)");
    int discipline = 0;
    detail::check(::ioctl(_impl->reader.native_handle(), TIOCGETD, &discipline), "TIOCGETD(input end)");
    const auto eof = settings.c_cc[VEOF];
    if (discipline != N_TTY || !(settings.c_lflag & ICANON)
        || eof == _POSIX_VDISABLE || (settings.c_lflag & EXTPROC)) {
        return std::nullopt;
    }
    // Reject settings that consume/transform the EOF byte before canonical
    // EOF handling. Do not inject a signal, erase key or flow-control command
    // while reporting successful end-input merely because ICANON is present.
    const auto matches = [&](int index) {
        return settings.c_cc[index] != _POSIX_VDISABLE && eof == settings.c_cc[index];
    };
    if (eof == '\n' || ((settings.c_iflag & ISTRIP) && eof > 127)
        || (eof == '\r' && (settings.c_iflag & (IGNCR | ICRNL)))
        || ((settings.c_iflag & IUCLC) && (settings.c_lflag & IEXTEN) && eof >= 'A' && eof <= 'Z')
        || matches(VERASE) || matches(VKILL) || matches(VEOL)
        || ((settings.c_lflag & IEXTEN) && matches(VEOL2))
        || ((settings.c_lflag & ISIG) && (matches(VINTR) || matches(VQUIT) || matches(VSUSP)))
        || ((settings.c_iflag & IXON) && (matches(VSTART) || matches(VSTOP)))
        || ((settings.c_lflag & IEXTEN) && (matches(VLNEXT) || matches(VWERASE) || matches(VREPRINT)))) {
        return std::nullopt;
    }
    return std::string(2, static_cast<char>(eof));
#else
    return std::nullopt;
#endif
}
void Terminal::resize(unsigned columns, unsigned rows)
{
#ifdef __linux__
    constexpr auto maximum = std::numeric_limits<unsigned short>::max();
    if (columns == 0 || rows == 0 || columns > maximum || rows > maximum) {
        throw std::invalid_argument("terminal dimensions must be in [1, 65535]");
    }
    winsize size{};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);
    detail::check(::ioctl(_impl->reader.native_handle(), TIOCSWINSZ, &size), "TIOCSWINSZ");
#else
    (void)columns;
    (void)rows;
    throw std::runtime_error("terminal resize is unsupported");
#endif
}
void Terminal::cancel_input(boost::system::error_code& error) noexcept
{
    error.clear();
#ifdef __linux__
    if (_impl->writer.is_open()) _impl->writer.close(error);
#endif
}
void Terminal::close(boost::system::error_code& error) noexcept
{
    cancel_input(error);
#ifdef __linux__
    boost::system::error_code other;
    if (_impl->reader.is_open()) _impl->reader.close(other);
    _impl->slave.reset();
    if (!error) error = other;
#endif
}
} // namespace subprocess
