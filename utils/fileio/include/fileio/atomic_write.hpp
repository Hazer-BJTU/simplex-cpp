#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>

namespace fileio {

/**
 * Render a file and atomically replace its destination after successful writing.
 *
 * @param file Destination path. Relative paths use the caller's current working
 * directory. Missing parent directories are created. The temporary file resides
 * in that directory, so publication does not cross filesystem boundaries.
 * @param write Synchronous producer called once with a writable binary stream
 * after temporary-file creation succeeds. It may stream arbitrary bytes without
 * constructing the whole file in memory. Do not retain the stream, replace its
 * buffer, or use it after the callback returns. A callback exception propagates
 * unchanged and prevents publication. An empty callback is an error.
 *
 * The POSIX implementation exclusively creates a mode-0600 temporary file,
 * flushes and fsyncs its contents, closes it, renames it over the destination,
 * and fsyncs the parent directory. Existing file permissions are replaced by
 * mode 0600. A destination symlink is replaced rather than followed. No previous
 * destination is modified on failure before rename; temporary files are removed
 * during normal exception unwinding. Filesystem/stream/system errors propagate.
 * If the directory sync fails after publication, the system error explicitly
 * says that the file has already been replaced. Newly created ancestor
 * directories are not individually synced; power-loss durability for their
 * creation is not guaranteed. Forced process termination can leave a temporary
 * file named .simplex-write-XXXXXX; abandoned-file cleanup is caller-owned.
 *
 * Independent calls use independent descriptors and temporary files. Concurrent
 * writers to the same destination need caller coordination: replacement is
 * atomic, but it neither merges contents nor detects another writer's update.
 * Internal descriptor, temporary-path, and stream-buffer ownership stays within
 * this utility; callers do not need POSIX or application-specific dependencies.
 */
void atomic_write(
    const std::filesystem::path& file,
    const std::function<void(std::ostream&)>& write);

} // namespace fileio
