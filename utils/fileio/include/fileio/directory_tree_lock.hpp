#pragma once

#include <filesystem>
#include <vector>

namespace fileio {

/**
 * Nonblocking POSIX coordination for mutations below an absolute directory.
 * Locks existing ancestors shared, top down, and the deepest existing directory
 * exclusive. Retain this guard across validation and creation/removal below the
 * requested directory. Thus a concurrent ancestor/descendant mutation cannot
 * pass validation independently, even when intermediate directories do not yet
 * exist. Unrelated trees can proceed concurrently.
 *
 * Uses directory inode flock locks, requires readable ancestors, and creates no
 * lock files in system directories. Cooperating operations must use this same
 * protocol. This does not protect against arbitrary filesystem writers or
 * provide distributed-filesystem locking. Busy acquisition throws system_error;
 * no operation waits while holding a partial lock chain.
 */
class DirectoryTreeLock {
public:
    explicit DirectoryTreeLock(const std::filesystem::path& directory);
    ~DirectoryTreeLock();
    DirectoryTreeLock(const DirectoryTreeLock&) = delete;
    DirectoryTreeLock& operator=(const DirectoryTreeLock&) = delete;

private:
    std::vector<int> descriptors_;
};

} // namespace fileio
