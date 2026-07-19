#include "indextools/cache_system.hpp"

namespace indextools {

CacheSystem::CacheSystem(size_t num_tasks, boost::asio::any_io_executor executor): 
_num_tasks(std::max<size_t>(num_tasks, 1u)), _executor(executor), _cache_strand(executor), _cache() {}

boost::asio::awaitable<CacheSystem::AnalyzePtr> CacheSystem::_get_entry(const std::filesystem::path& absolute_path) const noexcept {
    co_await boost::asio::dispatch(_cache_strand, boost::asio::use_awaitable);

    try {
        auto it = _cache.find(absolute_path);
        if (it == _cache.end()) {
            co_return nullptr;
        }

        auto current_filesize = std::filesystem::file_size(absolute_path);
        if (current_filesize != it->second.file_size) {
            co_return nullptr;
        }

        auto current_timestamp = std::filesystem::last_write_time(absolute_path);
        if (current_timestamp > it->second.last_modify) {
            co_return nullptr;
        }

        co_return it->second.content;
    } catch(...) {
        co_return nullptr;
    }
}

boost::asio::awaitable<void> CacheSystem::_write_entry(const std::filesystem::path& absolute_path, CacheEntry entry) noexcept {
    co_await boost::asio::dispatch(_cache_strand, boost::asio::use_awaitable);
    _cache[absolute_path] = std::move(entry);
    co_return;
}

// ============================================================================
// SearchInterface — glob-scoped batch queries over the analyzer cache
// ============================================================================

boost::asio::awaitable<nlohmann::json> SearchInterface::locate_pattern(
    const std::filesystem::path& root_path,
    const std::string& glob_pattern,
    std::string pattern,
    bool use_regex) {
    auto paths = glob_find(root_path, glob_pattern);
    co_return co_await _cache->launch_search(
        paths,
        [pattern = std::move(pattern), use_regex](const CacheSystem::AnalyzePtr& analyzer) {
            return analyzer->locate_pattern(pattern, use_regex);
        });
}

boost::asio::awaitable<nlohmann::json> SearchInterface::locate_identifier(
    const std::filesystem::path& root_path,
    const std::string& glob_pattern,
    std::string identifier) {
    auto paths = glob_find(root_path, glob_pattern);
    co_return co_await _cache->launch_search(
        paths,
        [identifier = std::move(identifier)](const CacheSystem::AnalyzePtr& analyzer) {
            return analyzer->locate_identifier(identifier);
        });
}

boost::asio::awaitable<nlohmann::json> SearchInterface::locate_entity(
    const std::filesystem::path& root_path,
    const std::string& glob_pattern,
    std::string entity_key) {
    auto paths = glob_find(root_path, glob_pattern);
    co_return co_await _cache->launch_search(
        paths,
        [entity_key = std::move(entity_key)](const CacheSystem::AnalyzePtr& analyzer) {
            return analyzer->locate_entity(entity_key);
        });
}

}
