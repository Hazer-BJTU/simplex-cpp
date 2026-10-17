#pragma once

#include <filesystem>

namespace fileio {
/**
 * Nonblocking POSIX advisory ownership (exclusive by default) of a stable lock file.
 *
 * All writers must cooperate and use the same path on a local filesystem with
 * flock support. Never unlink or replace the lock file while it may be used.
 * The descriptor is close-on-exec so executed tools cannot retain ownership.
 * Destruction (including exception unwinding) releases ownership; the kernel
 * also releases it when the owning process exits. This is not a distributed lock.
 */
class SessionLock {
public:
    /** Shared leases exclude management mutations while allowing state readers. */
    enum class Mode { Exclusive, Shared };
    explicit SessionLock(const std::filesystem::path& path, Mode mode = Mode::Exclusive);
    ~SessionLock();
    SessionLock(const SessionLock&) = delete;
    SessionLock& operator=(const SessionLock&) = delete;

private:
    int descriptor_ = -1;
};
}
