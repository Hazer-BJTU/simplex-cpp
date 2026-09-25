#include "fileio/read_prefix.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fileio {
namespace {

/// Own a read descriptor across allocation/read failures; never retry close.
class ReadDescriptor {
public:
    explicit ReadDescriptor(int value) : value_(value) {}
    ~ReadDescriptor() { ::close(value_); }
    ReadDescriptor(const ReadDescriptor&) = delete;
    ReadDescriptor& operator=(const ReadDescriptor&) = delete;
    int get() const noexcept { return value_; }

private:
    int value_;
};

/// Capture errno before constructing an exception can change it.
[[noreturn]] void fail(const char* operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}

} // namespace

std::string read_prefix(const std::filesystem::path& path, std::size_t max_bytes)
{
    if (path.native().find('\0') != std::string::npos) {
        throw std::invalid_argument("file path contains NUL");
    }
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        fail("open file prefix");
    }
    const ReadDescriptor file(descriptor);
    struct stat status {};
    if (::fstat(file.get(), &status) != 0) {
        fail("inspect file prefix");
    }
    if (!S_ISREG(status.st_mode)) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "file prefix requires a regular file");
    }

    std::string bytes;
    std::array<char, 8192> chunk{};
    while (bytes.size() < max_bytes) {
        const auto request = std::min(
            max_bytes - bytes.size(), chunk.size());
        const auto received = ::read(file.get(), chunk.data(), request);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("read file prefix");
        }
        if (received == 0) {
            break;
        }
        bytes.append(chunk.data(), static_cast<std::size_t>(received));
    }
    return bytes;
}

} // namespace fileio
