#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <system_error>

namespace fileio {

/**
 * A POSIX publication failure with the original error code.
 *
 * published() is true only when rename succeeded but directory synchronization
 * failed. The destination then contains the new file, with crash durability
 * uncertain. This flag describes this operation, not concurrent writers.
 */
class AtomicWriteError : public std::system_error {
public:
    AtomicWriteError(
        std::error_code code, const std::string& message, bool published = false);
    ~AtomicWriteError() override;

    bool published() const noexcept { return published_; }

private:
    bool published_;
};

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
 * The POSIX implementation exclusively creates a mode-0600, close-on-exec
 * temporary file, flushes and fsyncs its contents, closes it, renames it over the destination,
 * and fsyncs the parent directory. Existing file permissions are replaced by
 * mode 0600. A destination symlink is replaced rather than followed. No previous
 * destination is modified on failure before rename; temporary files are removed
 * during normal exception unwinding. Filesystem/stream/system errors propagate.
 * If the directory sync fails after publication, AtomicWriteError::published()
 * is true. All other utility failures occur before publication. Callback
 * exceptions propagate unchanged, including any publication flags belonging
 * to operations performed by the callback itself. Newly created ancestor
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
