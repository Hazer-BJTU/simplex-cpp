#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fileio {

/** The target changed after the caller read it; no replacement was published. */
class ReplaceConflict : public std::runtime_error {
public:
    explicit ReplaceConflict(const std::string& reason);
};

/** POSIX identity and mutation metadata observed on the opened file. */
struct FileIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t size = 0;
    std::uintmax_t mode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t group = 0;
    std::uintmax_t links = 0;
    std::intmax_t modified_seconds = 0;
    std::intmax_t changed_seconds = 0;
    long modified_nanoseconds = 0;
    long changed_nanoseconds = 0;

    bool operator==(const FileIdentity&) const = default;
};

/** Bytes and identity from one validated open, kept together through editing. */
struct EditableSnapshot {
    std::string bytes;
    FileIdentity identity;
};

/**
 * Read bytes and identity from one open of an existing single-link regular
 * file, without following its final symlink. Rejects an in-place change
 * observed during the read. Throws std::length_error when larger than
 * max_bytes. The parent path may still contain symlinks. This synchronous
 * read is not an atomic filesystem snapshot.
 */
[[nodiscard]] EditableSnapshot read_editable_file(
    const std::filesystem::path& path, std::size_t max_bytes);

/**
 * Atomically publish replacement bytes for an existing single-link regular file.
 *
 * Checks opened-file content and metadata against the initial snapshot, then
 * checks identity and metadata again immediately before rename. Rejects final
 * symlinks, files with multiple hard links, special mode bits, missing files
 * and conflicts.
 * Preserves owner, group and POSIX mode bits; other inode metadata, including
 * ACLs and extended attributes, is not preserved. An independent writer that
 * ignores this protocol may race after the final check; this is best-effort
 * conflict detection, not an atomic compare-and-swap transaction.
 *
 * The temporary file is created in the same directory and removed on any
 * failure before rename. An AtomicWriteError with published()==true means the
 * new contents are visible but directory synchronization failed. Other errors
 * occur before publication. No parent directories are created.
 */
void replace_existing_file(
    const std::filesystem::path& path,
    const EditableSnapshot& expected,
    std::string_view replacement);

} // namespace fileio
