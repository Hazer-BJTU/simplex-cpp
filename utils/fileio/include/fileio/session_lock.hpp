#pragma once

#include <filesystem>

namespace fileio {
/**
 * Exclusive, nonblocking POSIX advisory ownership of a stable lock file.
 *
 * All writers must cooperate and use the same path on a local filesystem with
 * flock support. Never unlink or replace the lock file while it may be used.
 * The descriptor is close-on-exec so executed tools cannot retain ownership.
 * Destruction (including exception unwinding) releases ownership; the kernel
 * also releases it when the owning process exits. This is not a distributed lock.
 */
class SessionLock {
public:
    explicit SessionLock(const std::filesystem::path& path);
    ~SessionLock();
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

private:
    int descriptor_ = -1;
};
}
