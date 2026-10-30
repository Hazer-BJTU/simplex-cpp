#include "fileio/staged_replace.hpp"

#include "fileio/atomic_write.hpp"
#include "fileio/cancellation.hpp"

#include "posix_identity.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fileio {
namespace {

constexpr std::size_t kMinBufferBytes = 4;
constexpr std::size_t kMaxBufferBytes = 1024 * 1024;
constexpr std::size_t kCompareChunk = 64 * 1024;

/** Read from both descriptors and require identical bytes. */
void compare_exact(
    int left, int right, std::uintmax_t bytes, std::uintmax_t base,
    SourceObserver* observer, std::stop_token stop)
{
    std::array<char, kCompareChunk> first{};
    std::array<char, kCompareChunk> second{};
    std::uintmax_t compared = 0;
    while (compared < bytes) {
        check_cancelled(stop);
        const auto want = static_cast<std::size_t>(
            std::min<std::uintmax_t>(first.size(), bytes - compared));
        const auto left_count = detail::read_some(left, first.data(), want);
        const auto right_count = detail::read_some(right, second.data(), want);
        if (left_count != right_count || left_count == 0) {
            throw ReplaceConflict("file changed before replacement");
        }
        if (std::memcmp(first.data(), second.data(), left_count) != 0) {
            throw ReplaceConflict("file contents changed before replacement");
        }
        if (observer != nullptr) {
            observer->observe(base + compared, {first.data(), left_count});
        }
        compared += left_count;
    }
}

/** Require `bytes` bytes of `descriptor` to equal `expected`. */
void compare_expected(
    int descriptor, std::uintmax_t bytes, std::string_view expected, std::stop_token stop)
{
    std::array<char, kCompareChunk> chunk{};
    std::uintmax_t consumed = 0;
    while (consumed < bytes) {
        check_cancelled(stop);
        const auto want = static_cast<std::size_t>(
            std::min<std::uintmax_t>(chunk.size(), bytes - consumed));
        const auto count = detail::read_some(descriptor, chunk.data(), want);
        if (count == 0 ||
            std::memcmp(chunk.data(), expected.data() + consumed, count) != 0) {
            throw ReplaceConflict("matched region changed before replacement");
        }
        consumed += count;
    }
}

/** Require both descriptors to end together with identical remaining bytes. */
void compare_to_end(
    int left, int right, std::uintmax_t base, SourceObserver* observer, std::stop_token stop)
{
    std::array<char, kCompareChunk> first{};
    std::array<char, kCompareChunk> second{};
    std::uintmax_t compared = 0;
    for (;;) {
        check_cancelled(stop);
        const auto left_count = detail::read_some(left, first.data(), first.size());
        const auto right_count = detail::read_some(right, second.data(), second.size());
        if (left_count != right_count) {
            throw ReplaceConflict("file length changed before replacement");
        }
        if (left_count == 0) return;
        if (std::memcmp(first.data(), second.data(), left_count) != 0) {
            throw ReplaceConflict("file contents changed before replacement");
        }
        if (observer != nullptr) observer->observe(base + compared, {first.data(), left_count});
        compared += left_count;
    }
}

/** Discard exactly `bytes` bytes from one descriptor. */
void skip_bytes(int descriptor, std::uintmax_t bytes, std::stop_token stop)
{
    std::array<char, kCompareChunk> chunk{};
    std::uintmax_t skipped = 0;
    while (skipped < bytes) {
        check_cancelled(stop);
        const auto want = static_cast<std::size_t>(
            std::min<std::uintmax_t>(chunk.size(), bytes - skipped));
        const auto count = detail::read_some(descriptor, chunk.data(), want);
        if (count == 0) throw ReplaceConflict("replacement region is missing");
        skipped += count;
    }
}

} // namespace

StagedReplacement::StagedReplacement(
    const std::filesystem::path& path, std::size_t buffer_bytes,
    std::stop_token stop)
    : stop_(stop)
{
    check_cancelled(stop_);
    if (buffer_bytes < kMinBufferBytes || buffer_bytes > kMaxBufferBytes) {
        throw std::invalid_argument("stream buffer must be between 4 bytes and 1 MiB");
    }
    const auto destination = detail::checked_path(path);
    detail::Descriptor source(detail::open_source(destination));
    const auto opened = detail::checked_regular(source.get());
    const auto parent = destination.parent_path();
    detail::Descriptor directory(
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!directory.valid()) detail::fail("open editable file directory");
    destination_ = destination;
    identity_ = detail::identity_of(opened);
    buffer_.resize(buffer_bytes);
    source_ = source.release();
    directory_ = directory.release();
}

StagedReplacement::StagedReplacement(StagedReplacement&& other) noexcept
    : destination_(std::move(other.destination_)),
      temporary_path_(std::move(other.temporary_path_)),
      identity_(other.identity_),
      buffer_(std::move(other.buffer_)),
      source_bytes_(other.source_bytes_),
      candidate_bytes_(other.candidate_bytes_),
      source_(std::exchange(other.source_, -1)),
      candidate_(std::exchange(other.candidate_, -1)),
      directory_(std::exchange(other.directory_, -1)),
      consumed_(std::exchange(other.consumed_, false)),
      stop_(other.stop_)
{
    other.temporary_path_.clear();
    other.source_bytes_ = 0;
    other.candidate_bytes_ = 0;
}

StagedReplacement& StagedReplacement::operator=(StagedReplacement&& other) noexcept
{
    if (this != &other) {
        discard();
        if (source_ >= 0) ::close(source_);
        if (directory_ >= 0) ::close(directory_);
        destination_ = std::move(other.destination_);
        temporary_path_ = std::move(other.temporary_path_);
        identity_ = other.identity_;
        buffer_ = std::move(other.buffer_);
        source_bytes_ = other.source_bytes_;
        candidate_bytes_ = other.candidate_bytes_;
        source_ = std::exchange(other.source_, -1);
        candidate_ = std::exchange(other.candidate_, -1);
        directory_ = std::exchange(other.directory_, -1);
        consumed_ = std::exchange(other.consumed_, false);
        stop_ = other.stop_;
        other.temporary_path_.clear();
        other.source_bytes_ = 0;
        other.candidate_bytes_ = 0;
    }
    return *this;
}

StagedReplacement::~StagedReplacement()
{
    discard();
    if (source_ >= 0) ::close(source_);
    if (directory_ >= 0) ::close(directory_);
}

void StagedReplacement::ensure_candidate()
{
    if (candidate_ >= 0) return;
    const auto parent = destination_.parent_path();
    auto pattern = (parent / ".simplex-edit-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    detail::Descriptor candidate(::mkostemp(name.data(), O_CLOEXEC));
    if (!candidate.valid()) detail::fail("create replacement file");
    // The path exists now; keep it removable even if the string assignment
    // below throws, because discard() would not know about it yet.
    struct Guard {
        const char* path;
        bool armed = true;
        ~Guard() { if (armed) ::unlink(path); }
    } guard{name.data()};
    temporary_path_ = name.data();
    guard.armed = false;
    candidate_ = candidate.release();
}

std::string_view StagedReplacement::next_source()
{
    check_cancelled(stop_);
    if (source_ < 0) return {};
    for (;;) {
        check_cancelled(stop_);
        const auto count = ::read(source_, buffer_.data(), buffer_.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            detail::fail("read editable file");
        }
        if (count == 0) return {};
        source_bytes_ += static_cast<std::uintmax_t>(count);
        return {buffer_.data(), static_cast<std::size_t>(count)};
    }
}

void StagedReplacement::restart_source()
{
    if (source_ < 0) throw std::logic_error("replacement source is closed");
    if (::lseek(source_, 0, SEEK_SET) != 0) detail::fail("rewind editable file");
    source_bytes_ = 0;
}

std::uintmax_t StagedReplacement::source_bytes() const noexcept
{
    return source_bytes_;
}

const FileIdentity& StagedReplacement::identity() const noexcept
{
    return identity_;
}

std::uintmax_t StagedReplacement::candidate_bytes() const noexcept
{
    return candidate_bytes_;
}

void StagedReplacement::copy_prefix(std::uintmax_t bytes)
{
    if (bytes > source_bytes_) {
        throw std::logic_error("prefix copy exceeds the scanned source");
    }
    if (bytes == 0) return;
    ensure_candidate();
    std::uintmax_t copied = 0;
#if defined(__linux__)
    // The kernel copies from the source without another user-space read, so the
    // scan budget stays at two source reads.
    while (copied < bytes) {
        check_cancelled(stop_);
        const auto want = static_cast<std::size_t>(
            std::min<std::uintmax_t>(bytes - copied, kCompareChunk));
        auto offset = static_cast<off_t>(copied);
        const auto count = ::copy_file_range(source_, &offset, candidate_, nullptr, want, 0);
        if (count > 0) {
            copied += static_cast<std::uintmax_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
#endif
    if (copied < bytes) {
        // Fallback for filesystems without copy_file_range; pread keeps the
        // source scan position untouched.
        std::array<char, kCompareChunk> chunk{};
        while (copied < bytes) {
            check_cancelled(stop_);
            const auto want = static_cast<std::size_t>(
                std::min<std::uintmax_t>(chunk.size(), bytes - copied));
            const auto count = ::pread(source_, chunk.data(), want, static_cast<off_t>(copied));
            if (count < 0) {
                if (errno == EINTR) continue;
                detail::fail("copy editable file prefix");
            }
            if (count == 0) detail::fail("copy editable file prefix");
            detail::write_all(candidate_, {chunk.data(), static_cast<std::size_t>(count)});
            copied += static_cast<std::uintmax_t>(count);
        }
    }
    candidate_bytes_ += bytes;
}

void StagedReplacement::write(std::string_view bytes)
{
    if (consumed_) throw std::logic_error("replacement candidate is closed");
    ensure_candidate();
    while (!bytes.empty()) {
        check_cancelled(stop_);
        const auto count = std::min(bytes.size(), kCompareChunk);
        detail::write_all(candidate_, bytes.substr(0, count));
        candidate_bytes_ += count;
        bytes.remove_prefix(count);
    }

}

void StagedReplacement::finish_candidate()
{
    ensure_candidate();
    struct stat created {};
    if (::fstat(candidate_, &created) != 0) detail::fail("inspect replacement file");
    if (static_cast<std::uintmax_t>(created.st_size) != candidate_bytes_) {
        throw std::system_error(
            std::make_error_code(std::errc::io_error),
            "replacement file does not match the written byte count");
    }
    const auto mode = static_cast<std::uintmax_t>(identity_.mode);
    if (created.st_uid != static_cast<uid_t>(identity_.owner) ||
        created.st_gid != static_cast<gid_t>(identity_.group)) {
        if (::fchown(candidate_, static_cast<uid_t>(identity_.owner),
                     static_cast<gid_t>(identity_.group)) != 0) {
            detail::fail("preserve editable file owner");
        }
    }
    if (::fchmod(candidate_, static_cast<mode_t>(mode & 0777)) != 0) {
        detail::fail("preserve editable file permissions");
    }
    check_cancelled(stop_);
    if (::fsync(candidate_) != 0) detail::fail("sync replacement file");
    if (::close(std::exchange(candidate_, -1)) != 0) {
        detail::fail("close replacement file");
    }
}

void StagedReplacement::confirm_source_unchanged(int descriptor) const
{
    struct stat current {};
    if (::fstat(descriptor, &current) != 0) detail::fail("inspect editable file");
    if (detail::identity_of(current) != identity_ ||
        static_cast<std::uintmax_t>(current.st_size) != source_bytes_) {
        throw ReplaceConflict("file changed during the edit");
    }
}

void StagedReplacement::verify_against_source(
    std::uintmax_t match_byte, std::string_view matched_source,
    SourceObserver* observer)
{
    if (match_byte > source_bytes_ ||
        matched_source.size() > source_bytes_ - match_byte) {
        throw ReplaceConflict("matched region is outside the scanned source");
    }
    if (candidate_bytes_ < source_bytes_ - matched_source.size()) {
        throw ReplaceConflict("replacement output is shorter than the removed span");
    }
    const auto replaced = candidate_bytes_ - (source_bytes_ - matched_source.size());
    // Reopen by path so the comparison also revalidates the current directory
    // entry; the descriptor check below rejects a replaced inode.
    detail::Descriptor reopened(detail::open_source(destination_));
    detail::checked_regular(reopened.get());
    confirm_source_unchanged(reopened.get());

    detail::Descriptor candidate(detail::open_source(temporary_path_));
    compare_exact(reopened.get(), candidate.get(), match_byte, 0, observer, stop_);
    skip_bytes(candidate.get(), replaced, stop_);
    compare_expected(reopened.get(), matched_source.size(), matched_source, stop_);
    compare_to_end(reopened.get(), candidate.get(), match_byte + matched_source.size(),
                   observer, stop_);

    struct stat after {};
    if (::fstat(reopened.get(), &after) != 0) detail::fail("inspect compared file");
    if (detail::identity_of(after) != identity_) {
        throw ReplaceConflict("file changed during comparison");
    }
}

void StagedReplacement::publish(
    std::uintmax_t match_byte, std::string_view matched_source,
    SourceObserver* observer)
{
    check_cancelled(stop_);
    if (source_ < 0 || consumed_) {
        throw std::logic_error("replacement candidate is already consumed");
    }
    // Nothing written yet is still a valid output (a file replaced by nothing),
    // so an empty candidate is created before verifying.
    ensure_candidate();
    finish_candidate();
    confirm_source_unchanged(source_);
    verify_against_source(match_byte, matched_source, observer);

    struct stat original {};
    if (::fstat(source_, &original) != 0) detail::fail("inspect editable file");
    struct stat current {};
    if (::lstat(destination_.c_str(), &current) != 0) {
        if (errno == ENOENT) throw ReplaceConflict("file disappeared before replacement");
        detail::fail("inspect file before replacement");
    }
    if (!detail::same_file(original, current)) {
        throw ReplaceConflict("file changed before replacement");
    }
    // Final cancellation boundary. Once entered, report publication (including
    // directory-sync failure) even if a stop races rename or fsync.
    check_cancelled(stop_);
    if (::rename(temporary_path_.c_str(), destination_.c_str()) != 0) {
        detail::fail("publish replacement file");
    }
    temporary_path_.clear();
    consumed_ = true;
    if (::fsync(directory_) != 0) {
        throw AtomicWriteError(
            {errno, std::generic_category()},
            "replacement published, but directory sync failed", true);
    }
}

void StagedReplacement::discard() noexcept
{
    if (candidate_ >= 0) {
        ::close(candidate_);
        candidate_ = -1;
    }
    if (!temporary_path_.empty()) {
        ::unlink(temporary_path_.c_str());
        temporary_path_.clear();
    }
}

} // namespace fileio
