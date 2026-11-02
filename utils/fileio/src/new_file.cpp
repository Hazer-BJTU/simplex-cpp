#include "fileio/new_file.hpp"
#include "fileio/atomic_write.hpp"
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace fileio {
namespace {
[[noreturn]] void fail(const char* operation, bool published = false) {
    throw AtomicWriteError({errno, std::generic_category()}, operation, published);
}
}
struct NewFile::Impl {
    int directory = -1;
    int file = -1;
    std::string temporary;
    std::string destination;
    bool published = false;
    ~Impl() {
        if (file >= 0) ::close(file);
        if (directory >= 0) {
            if (!temporary.empty()) ::unlinkat(directory, temporary.c_str(), 0);
            ::close(directory);
        }
    }
};
NewFile::NewFile(const std::filesystem::path& destination) : impl_(std::make_unique<Impl>()) {
    const auto path = std::filesystem::absolute(destination);
    if (path.native().find('\0') != std::string::npos || path.filename().empty()
        || path.filename() == "." || path.filename() == "..")
        throw std::invalid_argument("invalid export file path");
    impl_->directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (impl_->directory < 0) fail("open export parent directory");
    impl_->destination = path.filename().string();
    impl_->temporary = ".simplex-export-" + boost::uuids::to_string(boost::uuids::random_generator()());
    impl_->file = ::openat(impl_->directory, impl_->temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl_->file < 0) {
        impl_->temporary.clear(); // Never unlink an entry we did not create.
        fail("create export temporary file");
    }
}
NewFile::~NewFile() = default;
void NewFile::append(std::string_view bytes) {
    if (impl_->published) throw std::logic_error("export is already published");
    while (!bytes.empty()) {
        const auto written = ::write(impl_->file, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) fail("write export temporary file");
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
}
void NewFile::publish() {
    if (impl_->published) throw std::logic_error("export is already published");
    if (::fsync(impl_->file) != 0) fail("sync export file");
    if (::linkat(impl_->directory, impl_->temporary.c_str(), impl_->directory,
            impl_->destination.c_str(), 0) != 0) fail("publish export without overwrite");
    impl_->published = true;
    if (::unlinkat(impl_->directory, impl_->temporary.c_str(), 0) != 0)
        fail("export published but temporary cleanup failed", true);
    impl_->temporary.clear();
    if (::fsync(impl_->directory) != 0) fail("export published but directory sync failed", true);
}
}
