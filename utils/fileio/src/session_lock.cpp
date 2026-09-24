#include "fileio/session_lock.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <system_error>

namespace fileio {
SessionLock::SessionLock(const std::filesystem::path& path) {
    descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open session lock " + path.string());
    }
    if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        ::close(descriptor_);
        descriptor_ = -1;
        throw std::system_error(error, std::generic_category(),
            "cannot acquire exclusive session ownership: " + path.string());
    }
}

SessionLock::~SessionLock() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}
}
