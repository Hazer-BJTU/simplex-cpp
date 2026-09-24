#include "fileio/atomic_write.hpp"

#include <cerrno>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace fileio {
namespace fs = std::filesystem;
namespace {

/** Preserve the failing POSIX operation's errno in a normal exception. */
[[noreturn]] void system_failure(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

/** Own a descriptor, including unwinding paths; close is never retried. */
class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    int get() const noexcept { return value_; }

    void close() {
        const int value = std::exchange(value_, -1);
        if (::close(value) != 0) {
            system_failure("close file");
        }
    }

private:
    int value_;
};

/** Remove an unpublished temporary path on both IO and rendering failures. */
struct TemporaryPath {
    const char* path;
    bool published = false;
    ~TemporaryPath() {
        if (!published) {
            ::unlink(path);
        }
    }
};

/** Buffered stream output directly to the exclusively opened temporary file. */
class FileBuffer final : public std::streambuf {
public:
    explicit FileBuffer(int descriptor) : descriptor_(descriptor) {
        setp(buffer_, buffer_ + sizeof(buffer_));
    }

protected:
    int sync() override {
        const char* position = pbase();
        while (position != pptr()) {
            const auto written = ::write(
                descriptor_, position, static_cast<std::size_t>(pptr() - position));
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                return -1;
            }
            position += written;
        }
        setp(buffer_, buffer_ + sizeof(buffer_));
        return 0;
    }

    int_type overflow(int_type value) override {
        if (sync() != 0) {
            return traits_type::eof();
        }
        if (!traits_type::eq_int_type(value, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(value);
            pbump(1);
        }
        return traits_type::not_eof(value);
    }

private:
    int descriptor_;
    char buffer_[8192];
};

} // namespace

/** Publish only a completely rendered and flushed file on the same filesystem. */
void atomic_write(const fs::path& file, const std::function<void(std::ostream&)>& write) {
    if (!write) {
        throw std::invalid_argument("atomic_write requires a writer callback");
    }
    const auto destination = fs::absolute(file);
    const auto parent = destination.parent_path();
    fs::create_directories(parent);
    Descriptor directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) {
        system_failure("open file directory");
    }

    auto name = (parent / ".simplex-write-XXXXXX").string();
    std::vector<char> pattern(name.begin(), name.end());
    pattern.push_back('\0');
    Descriptor descriptor(::mkstemp(pattern.data()));
    if (descriptor.get() < 0) {
        system_failure("create temporary file");
    }
    TemporaryPath temporary{pattern.data()};
    FileBuffer buffer(descriptor.get());
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    write(output);
    output.flush();
    // A callback may change the exception mask; a failed stream still must
    // never publish an incomplete destination.
    if (!output) {
        throw std::ios_base::failure("cannot write temporary file");
    }
    if (::fsync(descriptor.get()) != 0) {
        system_failure("sync file");
    }
    descriptor.close();
    fs::rename(temporary.path, destination);
    temporary.published = true;
    if (::fsync(directory.get()) != 0) {
        system_failure("file replaced, but directory sync failed");
    }
}

} // namespace fileio
