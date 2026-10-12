#include "terminal_launch.hpp"
#include "native_fd.hpp"
#include <utility>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#ifdef __linux__
#include <climits>
#include <csignal>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#endif

namespace subprocess {
#ifdef __linux__
namespace {
/// Fixed-size child-to-parent diagnostic record; no child-side string formatting.
enum class LaunchStep : int {
    Session,
    ControllingTerminal,
    ForegroundGroup,
    Stdio,
    Directory,
    Descriptors,
    Exec
};
struct LaunchError {
    LaunchStep step;
    int code;
};

// The child uses only async-signal-safe native operations after fork. The error
// pipe and slave are always above fd 2; no stdio remapping can overwrite them.
[[noreturn]] void fail_child(int pipe, LaunchStep step) noexcept
{
    const LaunchError failure{step, errno};
    const auto* cursor = reinterpret_cast<const char*>(&failure);
    std::size_t left = sizeof(failure);
    while (left != 0) {
        const auto count = ::write(pipe, cursor, left);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        cursor += count;
        left -= static_cast<std::size_t>(count);
    }
    ::_exit(127);
}
// Older kernels lack close_range(CLOEXEC). Enumerate this fork's fixed table
// with native getdents64 rather than scanning RLIMIT_NOFILE: that limit can be
// enormous, or lowered below descriptors which are still open. No libc DIR,
// heap allocation or C++ locks are used in the child.
bool mark_inherited_descriptors() noexcept
{
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */) == 0) {
        return true;
    }
#endif
    const int directory = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        return false;
    }
    struct Entry {
        std::uint64_t inode;
        std::int64_t offset;
        unsigned short length;
        unsigned char type;
        char name[1];
    };
    alignas(Entry) char buffer[4096];
    bool success = true;
    for (;;) {
        const auto count = ::syscall(SYS_getdents64, directory, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            success = false;
            break;
        }
        if (count == 0) {
            break;
        }
        for (long position = 0; position < count;) {
            const auto* entry = reinterpret_cast<const Entry*>(buffer + position);
            // Records are supplied by the kernel; retain an explicit bound so
            // malformed/interposed results cannot walk past this stack buffer.
            if (count - position < static_cast<long>(offsetof(Entry, name) + 1)
                || entry->length < offsetof(Entry, name) + 1 || entry->length > count - position) {
                errno = EIO;
                success = false;
                break;
            }
            int fd = 0;
            bool numeric = true;
            bool terminated = false;
            const auto name_size = entry->length - offsetof(Entry, name);
            const char* name = buffer + position + offsetof(Entry, name);
            for (std::size_t i = 0; i < name_size; ++i) {
                const char c = name[i];
                if (c == 0) {
                    terminated = true;
                    break;
                }
                if (c < '0' || c > '9' || fd > (INT_MAX - (c - '0')) / 10) {
                    numeric = false;
                    break;
                }
                fd = fd * 10 + c - '0';
            }
            if (numeric && terminated && fd >= 3 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
                success = false;
                break;
            }
            position += entry->length;
        }
        if (!success) {
            break;
        }
    }
    const int error = errno;
    ::close(directory);
    errno = error;
    return success;
}

/// Synchronous rollback for a failed launch; EINTR does not abandon the child.
void reap(pid_t pid) noexcept
{
    while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
    }
}

/// Render the recorded setup step in the parent after child cleanup completes.
const char* step_name(LaunchStep step) noexcept
{
    switch (step) {
        case LaunchStep::Session:
            return "PTY setsid";
        case LaunchStep::ControllingTerminal:
            return "PTY TIOCSCTTY";
        case LaunchStep::ForegroundGroup:
            return "PTY foreground process group";
        case LaunchStep::Stdio:
            return "PTY dup2";
        case LaunchStep::Directory:
            return "PTY chdir";
        case LaunchStep::Descriptors:
            return "PTY close-on-exec descriptors";
        case LaunchStep::Exec:
            return "PTY execve";
    }
    return "PTY launch";
}
}
#endif
std::unique_ptr<boost::process::process> launch_terminal(
    boost::asio::any_io_executor executor,
    const boost::process::v2::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::vector<boost::process::environment::key_value_pair>& environment,
    const std::optional<std::string>& directory,
    int slave)
{
#ifdef __linux__
    // Materialize every pointer in the parent, before fork.
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    std::vector<char*> env;
    env.reserve(environment.size() + 1);
    for (const auto& value : environment) {
        env.push_back(const_cast<char*>(value.data()));
    }
    env.push_back(nullptr);
    const char* path = executable.c_str();
    const char* cwd = directory ? directory->c_str() : nullptr;
    char* const* args = argv.data();
    char* const* vars = env.data();
    int pipe[2];
    detail::check(::pipe2(pipe, O_CLOEXEC), "PTY launch error pipe");
    detail::Fd reader(pipe[0]), writer(pipe[1]);
    detail::move_above_stdio(reader);
    detail::move_above_stdio(writer);
    const int read_fd = reader.get(), write_fd = writer.get();
    const pid_t pid = ::fork();
    detail::check(pid, "PTY fork");
    if (pid == 0) {
        ::close(read_fd);
        if (::setsid() < 0) {
            fail_child(write_fd, LaunchStep::Session);
        }
        if (::ioctl(slave, TIOCSCTTY, 0) < 0) {
            fail_child(write_fd, LaunchStep::ControllingTerminal);
        }
        const pid_t group = ::getpid();
        if (::ioctl(slave, TIOCSPGRP, &group) < 0) {
            fail_child(write_fd, LaunchStep::ForegroundGroup);
        }
        for (int target = 0; target < 3; ++target) {
            if (::dup2(slave, target) < 0) {
                fail_child(write_fd, LaunchStep::Stdio);
            }
        }
        if (cwd && ::chdir(cwd) < 0) {
            fail_child(write_fd, LaunchStep::Directory);
        }
        // Preserve the error channel until exec, leaking no other host handle.
        if (!mark_inherited_descriptors()) {
            fail_child(write_fd, LaunchStep::Descriptors);
        }
        ::execve(path, args, vars);
        fail_child(write_fd, LaunchStep::Exec);
    }
    writer.reset();
    // EOF with no record means exec closed the child's CLOEXEC error writer.
    // Any bytes, including a partial record, mean launch cannot be committed.
    // Drop the parent's writer first so it cannot keep this blocking read alive.
    LaunchError failure{};
    std::size_t received = 0;
    int channel_error = 0;
    while (received < sizeof(failure)) {
        const auto count = ::read(
            reader.get(), reinterpret_cast<char*>(&failure) + received, sizeof(failure) - received);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            channel_error = errno;
            break;
        }
        if (count == 0) {
            break;
        }
        received += static_cast<std::size_t>(count);
    }
    if (channel_error || received != 0) {
        // A broken error channel cannot leave a launched, unowned child behind.
        ::kill(pid, SIGKILL);
        reap(pid);
        if (channel_error || received != sizeof(failure)) {
            throw boost::system::system_error(
                channel_error ? channel_error : EIO,
                boost::system::system_category(),
                "PTY launch error channel");
        }
        throw boost::system::system_error(
            failure.code, boost::system::system_category(), step_name(failure.step));
    }
    try {
        // Boost.Process becomes the sole lifecycle owner only after adoption.
        // Until then, this function must clean up the native child itself.
        return std::make_unique<boost::process::process>(std::move(executor), pid);
    } catch (...) {
        ::kill(pid, SIGKILL);
        reap(pid);
        throw;
    }
#else
    (void)executor;
    (void)executable;
    (void)arguments;
    (void)environment;
    (void)directory;
    (void)slave;
    throw std::runtime_error("POSIX PTY launch is supported on Linux only");
#endif
}
} // namespace subprocess
