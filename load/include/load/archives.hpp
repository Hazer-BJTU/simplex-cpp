#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace load {

/** Automatic compact-archive retention. Zero disables the respective limit. */
struct MemoryRetention {
    std::size_t max_archives = 20;
    std::uintmax_t max_bytes = 256 * 1024 * 1024;
    std::size_t max_age_days = 30;
};

struct ArchiveCleanup {
    std::size_t removed_archives = 0;
    std::uintmax_t removed_bytes = 0;
};

/**
 * Remove oldest recognized compact archives after a successful state commit.
 *
 * The caller must hold exclusive session ownership and ensure no archive writer
 * or tool runs concurrently. The current archive is always retained, even when
 * it exceeds a limit. Other archives are considered newest first, subject to
 * all enabled limits. Age uses state.md's modification time.
 *
 * Only ordinary directories with the worker's ordinal/timestamp/UUID name and
 * exactly one regular state.md file are eligible. Symlinks and unfamiliar
 * contents are left alone; deletion is never recursive. This is cooperative
 * storage management, not protection against hostile concurrent filesystem edits.
 * Filesystem errors throw after any completed deletions; callers must report
 * cleanup failure without undoing the already committed conversation.
 */
ArchiveCleanup prune_memory_archives(
    const std::filesystem::path& directory,
    const std::filesystem::path& current_archive,
    const MemoryRetention& policy);

} // namespace load
