#pragma once

// Private POSIX helpers shared by the file-replacement implementations in this
// directory. Nothing here is part of the installed interface: callers outside
// fileio continue to see the path-based API and the FileIdentity value type.

#include "fileio/replace_existing.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fileio::detail {

/// Capture errno before constructing an exception can change it.
[[noreturn]] inline void fail(const char* operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}

/// Own a descriptor on all exits, including exceptions while writing.
class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept
    {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }

    /// Give up ownership so a caller object can adopt the descriptor.
    [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }

    /// Close eagerly so a later failure happens before publication, not in a
    /// destructor.
    void close()
    {
        const auto value = std::exchange(value_, -1);
        if (::close(value) != 0) fail("close replacement file");
    }

private:
    int value_;
};

/// Reject NUL paths early and resolve relative paths once, at the call.
inline std::filesystem::path checked_path(const std::filesystem::path& path)
{
    if (path.native().find('\0') != std::string::npos) {
        throw std::invalid_argument("file path contains NUL");
    }
    return std::filesystem::absolute(path);
}

/// Require the editable-file policy: single-link regular file, no set-id bits.
inline struct stat checked_regular(int descriptor)
{
    struct stat info {};
    if (::fstat(descriptor, &info) != 0) fail("inspect editable file");
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        (info.st_mode & 07000) != 0) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "editing requires a single-link regular file without special mode bits");
    }
    return info;
}

/// Open for editing without following a final symlink.
inline int open_source(const std::filesystem::path& path)
{
    const auto descriptor = ::open(
        path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) fail("open editable file");
    return descriptor;
}

/// Compare the fields that reveal ordinary concurrent content/identity changes.
inline bool same_file(const struct stat& left, const struct stat& right)
{
    return S_ISREG(right.st_mode) && right.st_nlink == 1 &&
           left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_size == right.st_size && left.st_mode == right.st_mode &&
           left.st_uid == right.st_uid && left.st_gid == right.st_gid &&
           left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

inline FileIdentity identity_of(const struct stat& info)
{
    return {
        static_cast<std::uintmax_t>(info.st_dev),
        static_cast<std::uintmax_t>(info.st_ino),
        static_cast<std::uintmax_t>(info.st_size),
        static_cast<std::uintmax_t>(info.st_mode),
        static_cast<std::uintmax_t>(info.st_uid),
        static_cast<std::uintmax_t>(info.st_gid),
        static_cast<std::uintmax_t>(info.st_nlink),
        static_cast<std::intmax_t>(info.st_mtim.tv_sec),
        static_cast<std::intmax_t>(info.st_ctim.tv_sec),
        info.st_mtim.tv_nsec,
        info.st_ctim.tv_nsec
    };
}

/// Write every byte, retrying interrupted calls and rejecting zero progress.
inline void write_all(int descriptor, std::string_view bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + offset,
                                   bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) fail("write replacement file");
        offset += static_cast<std::size_t>(count);
    }
}

/**
 * Fill `data` with exactly `count` bytes.
 *
 * Returns the number of bytes actually read, which is below `count` only at
 * EOF. Interrupted calls are retried; IO failures throw.
 */
inline std::size_t read_some(int descriptor, char* data, std::size_t count)
{
    std::size_t offset = 0;
    while (offset < count) {
        const auto received = ::read(descriptor, data + offset, count - offset);
        if (received < 0) {
            if (errno == EINTR) continue;
            fail("read replacement file");
        }
        if (received == 0) break;
        offset += static_cast<std::size_t>(received);
    }
    return offset;
}

} // namespace fileio::detail
