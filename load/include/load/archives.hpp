#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace load {

/** Automatic compact-archive retention. Zero disables cleanup. */
struct MemoryRetention {
    std::size_t max_archives = 5;
};

struct ArchiveCleanup {
    std::size_t removed_archives = 0;
    std::uintmax_t removed_bytes = 0;
};

/**
 * Remove oldest recognized compact archives after a settled compact attempt.
 *
 * The caller must hold exclusive session ownership and ensure no archive writer
 * or tool runs concurrently. The current archive is always retained and counts
 * toward max_archives. Additional referenced archives are also always retained;
 * protected archives take precedence even when their count exceeds the target.
 * Other recognized archives are retained newest first
 * until the count limit is reached.
 *
 * Only ordinary directories with the worker's ordinal/timestamp/UUID name and
 * exactly one regular state.md file are eligible. Symlinks and unfamiliar
 * contents are left alone; deletion is never recursive. This is cooperative
 * storage management, not protection against hostile concurrent filesystem edits.
 * Filesystem errors throw after any completed deletions; callers must report
 * cleanup failure without replacing the original outcome or undoing state.
 */
ArchiveCleanup prune_memory_archives(
    const std::filesystem::path& directory,
    const std::filesystem::path& current_archive,
    const MemoryRetention& policy,
    std::span<const std::filesystem::path> referenced_archives = {});

} // namespace load
