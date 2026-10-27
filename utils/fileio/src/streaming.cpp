#include "fileio/streaming.hpp"

#include "posix_identity.hpp"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fileio {
namespace {

constexpr std::size_t kMinBufferBytes = 4;
constexpr std::size_t kMaxBufferBytes = 1024 * 1024;

} // namespace

SequentialFile::SequentialFile(int descriptor, FileIdentity identity, std::size_t buffer_bytes)
    : descriptor_(descriptor), identity_(identity), buffer_(buffer_bytes)
{}

SequentialFile::SequentialFile(SequentialFile&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      identity_(other.identity_),
      buffer_(std::move(other.buffer_)),
      observed_bytes_(other.observed_bytes_)
{
    other.observed_bytes_ = 0;
}

SequentialFile& SequentialFile::operator=(SequentialFile&& other) noexcept
{
    if (this != &other) {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = std::exchange(other.descriptor_, -1);
        identity_ = other.identity_;
        buffer_ = std::move(other.buffer_);
        observed_bytes_ = std::exchange(other.observed_bytes_, 0);
    }
    return *this;
}

SequentialFile::~SequentialFile()
{
    if (descriptor_ >= 0) ::close(descriptor_);
}

SequentialFile SequentialFile::open(
    const std::filesystem::path& path, std::size_t buffer_bytes)
{
    if (buffer_bytes < kMinBufferBytes || buffer_bytes > kMaxBufferBytes) {
        throw std::invalid_argument("stream buffer must be between 4 bytes and 1 MiB");
    }
    if (path.native().find('\0') != std::string::npos) {
        throw std::invalid_argument("file path contains NUL");
    }
    // Nonblocking open keeps a substituted FIFO from waiting for a writer; the
    // descriptor check below then refuses anything that is not a regular file.
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) detail::fail("open streamed file");
    detail::Descriptor holder(descriptor);
    struct stat info {};
    if (::fstat(holder.get(), &info) != 0) detail::fail("inspect streamed file");
    if (!S_ISREG(info.st_mode)) {
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            "streaming read requires a regular file");
    }
    return SequentialFile(holder.release(), detail::identity_of(info), buffer_bytes);
}

std::string_view SequentialFile::fill()
{
    for (;;) {
        const auto count = ::read(descriptor_, buffer_.data(), buffer_.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            detail::fail("read streamed file");
        }
        if (count == 0) return {};
        observed_bytes_ += static_cast<std::uintmax_t>(count);
        return {buffer_.data(), static_cast<std::size_t>(count)};
    }
}

std::string_view SequentialFile::next()
{
    return fill();
}

std::uintmax_t SequentialFile::observed_bytes() const noexcept
{
    return observed_bytes_;
}

const FileIdentity& SequentialFile::identity() const noexcept
{
    return identity_;
}

} // namespace fileio
