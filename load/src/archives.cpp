#include "load/archives.hpp"

#include <algorithm>
#include <chrono>
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
    if (!policy.max_archives && !policy.max_bytes && !policy.max_age_days) {
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
        fs::file_time_type modified;
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
        archives.push_back({entry.path(), file.file_size(), file.last_write_time()});
    }
    const auto protected_entry = std::find_if(archives.begin(), archives.end(),
        [&](const auto& archive) { return archive.directory == current; });
    if (protected_entry == archives.end()) {
        throw std::runtime_error("current memory archive is missing or has unexpected contents");
    }
    std::uintmax_t bytes = protected_entry->bytes;
    std::size_t retained = 1;
    const auto now = fs::file_time_type::clock::now();
    std::sort(archives.begin(), archives.end(), [](const auto& left, const auto& right) {
        return left.directory.filename() > right.directory.filename();
    });
    for (const auto& archive : archives) {
        if (archive.directory == current) continue;
        const bool too_old = policy.max_age_days != 0
            && std::chrono::duration<double, std::ratio<86400>>(now - archive.modified).count()
                > static_cast<double>(policy.max_age_days);
        const bool too_many = policy.max_archives != 0 && retained >= policy.max_archives;
        const bool too_large = policy.max_bytes != 0
            && (bytes >= policy.max_bytes || archive.bytes > policy.max_bytes - bytes);
        if (too_old || too_many || too_large) {
            if (fs::remove(archive.directory / "state.md")) {
                result.removed_bytes += archive.bytes;
                ++result.removed_archives;
            }
            // Refuse to recursively delete unexpected files introduced since
            // the scan. A failed directory removal is reported to the caller.
            fs::remove(archive.directory);
        } else {
            ++retained;
            // Saturation avoids overflow even when the byte limit is disabled.
            bytes += std::min(archive.bytes, UINTMAX_MAX - bytes);
        }
    }
    return result;
}

} // namespace load
