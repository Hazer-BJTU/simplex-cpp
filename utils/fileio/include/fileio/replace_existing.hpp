#pragma once

#include <cstddef>
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

/**
 * Read an existing single-link regular file without following its final symlink.
 * Throws std::length_error when larger than max_bytes. The parent path may still
 * contain symlinks. Read is synchronous and not a filesystem snapshot.
 */
[[nodiscard]] std::string read_editable_file(
    const std::filesystem::path& path, std::size_t max_bytes);

/**
 * Atomically publish replacement bytes for an existing single-link regular file.
 *
 * Checks opened-file content against expected, then checks its identity and
 * metadata again immediately before rename. Rejects final symlinks, files
 * with multiple hard links, special mode bits, missing files and conflicts.
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
    std::string_view expected,
    std::string_view replacement);

} // namespace fileio
