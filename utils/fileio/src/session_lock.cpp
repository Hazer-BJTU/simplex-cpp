#include "fileio/session_lock.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <system_error>

namespace fileio {
SessionLock::SessionLock(const std::filesystem::path& path, Mode mode) {
    const bool shared = mode == Mode::Shared;
    // Shared inspection never writes: it must work on a read-only session
    // directory. An absent lock file is created so a later exclusive owner
    // shares the same inode instead of fencing a phantom file.
    descriptor_ = ::open(path.c_str(), (shared ? O_RDONLY : O_RDWR | O_CREAT) | O_CLOEXEC, 0600);
    if (descriptor_ < 0 && shared && errno == ENOENT) {
        descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    }
    if (descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open session lock " + path.string());
    }
    if (::flock(descriptor_, (shared ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) {
        const int error = errno;
        ::close(descriptor_);
        descriptor_ = -1;
        throw std::system_error(error, std::generic_category(),
            (shared ? "cannot acquire shared session lock: "
                    : "cannot acquire exclusive session ownership: ") + path.string());
    }
}

SessionLock::~SessionLock() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}
}
