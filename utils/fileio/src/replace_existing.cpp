#include "fileio/replace_existing.hpp"

#include "fileio/atomic_write.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fileio {
namespace {

[[noreturn]] void fail(const char* operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}

/** Own a descriptor on all exits, including exceptions while writing. */
class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return value_; }

    void close()
    {
        const auto value = std::exchange(value_, -1);
        if (::close(value) != 0) fail("close replacement file");
    }

private:
    int value_;
};

/** Delete only a temporary file that has not been renamed into place. */
struct TemporaryPath {
    std::string path;
    bool published = false;
    ~TemporaryPath() { if (!published) ::unlink(path.c_str()); }
};

std::filesystem::path checked_path(const std::filesystem::path& path)
{
    if (path.native().find('\0') != std::string::npos) {
        throw std::invalid_argument("file path contains NUL");
    }
    return std::filesystem::absolute(path);
}

struct stat checked_regular(int descriptor)
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

int open_source(const std::filesystem::path& path)
{
    const auto descriptor = ::open(
        path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) fail("open editable file");
    return descriptor;
}

/** Compare the fields that reveal ordinary concurrent content/identity changes. */
bool same_file(const struct stat& left, const struct stat& right)
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

FileIdentity identity_of(const struct stat& info)
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

void compare_expected(int descriptor, std::string_view expected)
{
    std::array<char, 8192> chunk{};
    std::size_t offset = 0;
    while (offset < expected.size()) {
        const auto request = std::min(chunk.size(), expected.size() - offset);
        const auto count = ::read(descriptor, chunk.data(), request);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fail("compare editable file");
        if (count == 0 || std::memcmp(chunk.data(), expected.data() + offset,
                                      static_cast<std::size_t>(count)) != 0) {
            throw ReplaceConflict("file contents changed before replacement");
        }
        offset += static_cast<std::size_t>(count);
    }
    char extra;
    for (;;) {
        const auto count = ::read(descriptor, &extra, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fail("compare editable file");
        if (count != 0) throw ReplaceConflict("file grew before replacement");
        break;
    }
}

void write_all(int descriptor, std::string_view bytes)
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

} // namespace

ReplaceConflict::ReplaceConflict(const std::string& reason)
    : std::runtime_error(reason) {}

EditableSnapshot read_editable_file(const std::filesystem::path& path, std::size_t max_bytes)
{
    if (max_bytes == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("editable file limit leaves no lookahead byte");
    }
    const Descriptor source(open_source(checked_path(path)));
    const auto opened = checked_regular(source.get());
    EditableSnapshot snapshot;
    snapshot.identity = identity_of(opened);
    auto& bytes = snapshot.bytes;
    std::array<char, 8192> chunk{};
    const auto limit = max_bytes + 1;
    while (bytes.size() < limit) {
        const auto request = std::min(chunk.size(), limit - bytes.size());
        const auto count = ::read(source.get(), chunk.data(), request);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) fail("read editable file");
        if (count == 0) break;
        bytes.append(chunk.data(), static_cast<std::size_t>(count));
    }
    if (bytes.size() > max_bytes) {
        throw std::length_error("editable file exceeds the configured byte limit");
    }
    struct stat after_read {};
    if (::fstat(source.get(), &after_read) != 0) fail("inspect read file");
    if (!same_file(opened, after_read) ||
        opened.st_size < 0 ||
        static_cast<std::uintmax_t>(opened.st_size) != bytes.size()) {
        throw ReplaceConflict("file changed during initial read");
    }
    return snapshot;
}

void replace_existing_file(
    const std::filesystem::path& path,
    const EditableSnapshot& expected,
    std::string_view replacement)
{
    const auto destination = checked_path(path);
    const Descriptor source(open_source(destination));
    const auto original = checked_regular(source.get());
    if (identity_of(original) != expected.identity) {
        throw ReplaceConflict("file identity changed after initial read");
    }
    if (original.st_size < 0 ||
        static_cast<std::uintmax_t>(original.st_size) != expected.bytes.size()) {
        throw ReplaceConflict("file size changed before replacement");
    }
    compare_expected(source.get(), expected.bytes);
    struct stat after_compare {};
    if (::fstat(source.get(), &after_compare) != 0) fail("inspect compared file");
    if (!same_file(original, after_compare)) {
        throw ReplaceConflict("file changed during comparison");
    }

    const auto parent = destination.parent_path();
    const Descriptor directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) fail("open editable file directory");
    auto pattern = (parent / ".simplex-edit-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    Descriptor temporary_file(::mkostemp(name.data(), O_CLOEXEC));
    if (temporary_file.get() < 0) fail("create replacement file");
    TemporaryPath temporary{name.data()};
    write_all(temporary_file.get(), replacement);
    struct stat created {};
    if (::fstat(temporary_file.get(), &created) != 0) fail("inspect replacement file");
    if (created.st_uid != original.st_uid || created.st_gid != original.st_gid) {
        if (::fchown(temporary_file.get(), original.st_uid, original.st_gid) != 0) {
            fail("preserve editable file owner");
        }
    }
    if (::fchmod(temporary_file.get(), original.st_mode & 0777) != 0) {
        fail("preserve editable file permissions");
    }
    if (::fsync(temporary_file.get()) != 0) fail("sync replacement file");
    temporary_file.close();

    struct stat current {};
    if (::lstat(destination.c_str(), &current) != 0) {
        if (errno == ENOENT) throw ReplaceConflict("file disappeared before replacement");
        fail("inspect file before replacement");
    }
    if (!same_file(original, current)) {
        throw ReplaceConflict("file changed before replacement");
    }
    if (::rename(name.data(), destination.c_str()) != 0) {
        fail("publish replacement file");
    }
    temporary.published = true;
    if (::fsync(directory.get()) != 0) {
        throw AtomicWriteError(
            {errno, std::generic_category()},
            "replacement published, but directory sync failed", true);
    }
}

} // namespace fileio
