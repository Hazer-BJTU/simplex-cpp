#pragma once
#include <filesystem>
#include <memory>
#include <string_view>

namespace fileio {
/** Stream a private temporary file, then publish without overwriting any entry.
 * The parent must exist. Operations are synchronous; async callers should use
 * a storage executor. The opened directory pins publication and cleanup even
 * when its pathname changes. Destruction removes unpublished temporary data.
 */
class NewFile {
public:
    explicit NewFile(const std::filesystem::path& destination);
    ~NewFile();
    NewFile(const NewFile&) = delete;
    NewFile& operator=(const NewFile&) = delete;
    void append(std::string_view bytes);
    /** fsync the file, atomically link its name exclusively, then sync directory.
     * AtomicWriteError::published() distinguishes failures after publication.
     * Existing files and symlinks are never followed or overwritten.
     */
    void publish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
