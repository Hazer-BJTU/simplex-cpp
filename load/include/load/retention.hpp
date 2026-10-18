#pragma once

#include <cstddef>

namespace load {

/**
 * Configured compact-archive retention target.
 *
 * `persistence.memory_retention.max_archives` parses into this value. Zero
 * disables cleanup. The current attempt and the archives that retained state
 * still names are always protected, so the target is a floor rather than a hard
 * limit. Applying the policy is a storage-backend operation: the backend knows
 * which on-disk or on-database artifacts are complete, recognized archives.
 */
struct MemoryRetention {
    std::size_t max_archives = 5;
};

} // namespace load
