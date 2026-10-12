#pragma once
#ifdef __linux__
#include <cerrno>
#include <utility>
#include <fcntl.h>
#include <unistd.h>
#include <boost/system/system_error.hpp>

namespace subprocess::detail {
/// Temporary Linux descriptor owner for allocation and fork/exec setup.
/// Ownership transfers to Asio only after successful assign(); release() then
/// prevents the temporary guard from closing the adopted descriptor.
class Fd {
public:
    explicit Fd(int value = -1) noexcept
        : value(value)
    {
    }
    ~Fd()
    {
        reset();
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const noexcept
    {
        return value;
    }
    int release() noexcept
    {
        return std::exchange(value, -1);
    }
    void reset(int next = -1) noexcept
    {
        const int old = std::exchange(value, next);
        if (old >= 0) {
            ::close(old); // Linux close must not be retried on EINTR.
        }
    }

private:
    int value;
};
/// Preserve native errno at the failure site with an operation-specific message.
inline void check(int result, const char* operation)
{
    if (result < 0) {
        throw boost::system::system_error(errno, boost::system::system_category(), operation);
    }
}
/// Launch resources never occupy standard descriptors, even if the host has
/// closed them. This also protects the private exec-error channel from dup2.
inline void move_above_stdio(Fd& fd)
{
    check(fd.get(), "allocate terminal descriptor");
    if (fd.get() < 3) {
        int copy = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 3);
        check(copy, "relocate terminal descriptor");
        fd.reset(copy);
    }
}
} // namespace subprocess::detail
#endif
