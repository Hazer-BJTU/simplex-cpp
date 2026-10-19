#include "fileio/directory_tree_lock.hpp"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>

namespace fileio {

DirectoryTreeLock::DirectoryTreeLock(const std::filesystem::path& directory) {
    if (!directory.is_absolute() || directory != directory.lexically_normal()) {
        throw std::invalid_argument("directory lock requires a normalized absolute path");
    }
    try {
        std::filesystem::path current;
        for (const auto& part : directory) {
            current /= part;
            const int fd = ::open(current.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) {
                const int error = errno;
                if (error == ENOENT && !descriptors_.empty()) {
                    // Upgrade before making any filesystem changes. flock's
                    // conversion need not be atomic: failure aborts the whole
                    // operation, and callers validate only after success.
                    if (::flock(descriptors_.back(), LOCK_EX | LOCK_NB) != 0) {
                        throw std::system_error(errno, std::generic_category(), "lock directory ancestor");
                    }
                    break;
                }
                if (error == ELOOP || error == ENOTDIR) {
                    // O_NOFOLLOW reports a symlink ancestor as ELOOP, while
                    // O_NOFOLLOW|O_DIRECTORY reports it as ENOTDIR. Name the
                    // requirement instead of surfacing the raw errno.
                    throw std::invalid_argument(
                        "directory lock requires ancestors that are ordinary directories, "
                        "not symlinks or files: " + current.string());
                }
                throw std::system_error(error, std::generic_category(), "open directory ancestor");
            }
            try {
                descriptors_.push_back(fd);
            } catch (...) {
                ::close(fd);
                throw;
            }
            const int mode = current == directory ? LOCK_EX : LOCK_SH;
            if (::flock(fd, mode | LOCK_NB) != 0) {
                throw std::system_error(errno, std::generic_category(), "lock directory ancestor");
            }
        }
    } catch (...) {
        for (int fd : descriptors_) {
            ::close(fd);
        }
        throw;
    }
}

DirectoryTreeLock::~DirectoryTreeLock() {
    for (auto it = descriptors_.rbegin(); it != descriptors_.rend(); ++it) {
        ::close(*it);
    }
}

} // namespace fileio
