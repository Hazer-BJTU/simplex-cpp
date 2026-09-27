#include "load/archives.hpp"

#include <algorithm>
#include <regex>
#include <stdexcept>
#include <vector>

namespace load {

ArchiveCleanup prune_memory_archives(
    const std::filesystem::path& directory,
    const std::filesystem::path& current_archive,
    const MemoryRetention& policy
) {
    namespace fs = std::filesystem;
    ArchiveCleanup result;
    if (policy.max_archives == 0) {
        return result;
    }
    const auto root = fs::absolute(directory).lexically_normal();
    const auto current = fs::absolute(current_archive).lexically_normal();
    if (current.parent_path() != root || !fs::is_directory(fs::symlink_status(root))) {
        throw std::invalid_argument("memory cleanup requires an ordinary archive root");
    }
    static const std::regex name_pattern(
        R"(^[0-9]{20}-[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{6}Z-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)");
    struct Archive {
        fs::path directory;
        std::uintmax_t bytes;
    };
    std::vector<Archive> archives;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!std::regex_match(entry.path().filename().string(), name_pattern)
            || !fs::is_directory(entry.symlink_status())) {
            continue;
        }
        auto child = fs::directory_iterator(entry.path());
        if (child == fs::directory_iterator()) continue;
        const auto file = *child;
        if (file.path().filename() != "state.md"
            || !fs::is_regular_file(file.symlink_status())
            || ++child != fs::directory_iterator()) {
            continue;
        }
        archives.push_back({entry.path(), file.file_size()});
    }
    const auto protected_entry = std::find_if(archives.begin(), archives.end(),
        [&](const auto& archive) { return archive.directory == current; });
    if (protected_entry == archives.end()) {
        throw std::runtime_error("current memory archive is missing or has unexpected contents");
    }
    std::size_t retained = 1;
    std::sort(archives.begin(), archives.end(), [](const auto& left, const auto& right) {
        return left.directory.filename() > right.directory.filename();
    });
    for (const auto& archive : archives) {
        if (archive.directory == current) continue;
        if (retained >= policy.max_archives) {
            if (fs::remove(archive.directory / "state.md")) {
                result.removed_bytes += archive.bytes;
                ++result.removed_archives;
            }
            // Refuse to recursively delete unexpected files introduced since
            // the scan. A failed directory removal is reported to the caller.
            fs::remove(archive.directory);
        } else {
            ++retained;
        }
    }
    return result;
}

} // namespace load
