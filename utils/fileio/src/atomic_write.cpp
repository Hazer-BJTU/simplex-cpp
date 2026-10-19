#include "fileio/atomic_write.hpp"

#include <cerrno>
#include <cstdlib>
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
[[noreturn]] void system_failure(const char* operation, bool published = false) {
    throw AtomicWriteError({errno, std::generic_category()}, operation, published);
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

AtomicWriteError::AtomicWriteError(
    std::error_code code, const std::string& message, bool published)
    : std::system_error(code, message), published_(published) {}

AtomicWriteError::~AtomicWriteError() = default;

struct StagedWrite::Impl {
    fs::path destination;
    std::string temporary;
    Descriptor directory{-1};
    bool published = false;

    explicit Impl(const fs::path& file)
        : destination(fs::absolute(file)), directory(open_directory(destination.parent_path())) {}

    static int open_directory(const fs::path& parent) {
        fs::create_directories(parent);
        const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) system_failure("open file directory");
        return fd;
    }

    ~Impl() {
        if (!published && !temporary.empty()) ::unlink(temporary.c_str());
    }
};

StagedWrite::StagedWrite(const fs::path& file, const std::function<void(std::ostream&)>& write) {
    if (!write) throw std::invalid_argument("atomic_write requires a writer callback");
    impl_ = std::make_unique<Impl>(file);
    auto name = (impl_->destination.parent_path() / ".simplex-write-XXXXXX").string();
    std::vector<char> pattern(name.begin(), name.end());
    pattern.push_back('\0');
    Descriptor descriptor(::mkostemp(pattern.data(), O_CLOEXEC));
    if (descriptor.get() < 0) system_failure("create temporary file");
    TemporaryPath temporary{pattern.data()};
    impl_->temporary = pattern.data();
    temporary.published = true; // Impl now owns cleanup, including constructor failure.
    FileBuffer buffer(descriptor.get());
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    write(output);
    output.flush();
    if (!output) throw std::ios_base::failure("cannot write temporary file");
    if (::fsync(descriptor.get()) != 0) system_failure("sync file");
    descriptor.close();
}

StagedWrite::~StagedWrite() = default;

void StagedWrite::publish() {
    if (impl_->published) throw std::logic_error("staged file is already published");
    fs::rename(impl_->temporary, impl_->destination);
    impl_->published = true;
    if (::fsync(impl_->directory.get()) != 0) {
        system_failure("file replaced, but directory sync failed", true);
    }
}

void atomic_write(const fs::path& file, const std::function<void(std::ostream&)>& write) {
    StagedWrite staged(file, write);
    staged.publish();
}

} // namespace fileio
