#pragma once

/**
 * @file cache_system.hpp
 * @brief Analyzer cache and high-level batch search over the filesystem.
 *
 * This header exposes two cooperating types:
 *
 *   - CacheSystem: an mtime/size-keyed cache of parsed analyzers plus a
 *     coroutine-based, fan-out search engine (launch_search) that resolves a
 *     list of file paths to analyzers (reusing cached ones, parsing misses)
 *     and applies a caller-supplied query to each, merging the per-file JSON
 *     results into one array.
 *
 *   - SearchInterface: a thin facade over CacheSystem whose queries are keyed
 *     on a (root directory, glob pattern) pair rather than an explicit path
 *     list. It expands the glob via indextools::glob_find() and forwards the
 *     resulting paths to CacheSystem::launch_search, exposing the three query
 *     levels the rest of the system speaks in — locate_pattern (incl. regex),
 *     locate_identifier and locate_entity.
 *
 * ## Concurrency model
 *
 * All cache mutation is serialized through a strand (_cache_strand), so the
 * underlying unordered_map is only ever touched from one logical thread of
 * execution even when many search tasks run concurrently on the executor.
 * launch_search spawns up to _num_tasks child coroutines, each owning a
 * contiguous slice of the (deduplicated, canonicalized) path list, and collects
 * their partial results over an asio concurrent_channel. The concurrent variant
 * is required because the spawned coroutines run on the raw executor (not a
 * strand): under a multi-threaded io_context::run() they may execute and reach
 * their async_send from different threads concurrently, and a plain channel is
 * not safe for concurrent senders.
 */

#include "indextools/plugin_manager.hpp"
#include "indextools/utils.hpp"

#include <ranges>

#include <boost/asio.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include <nlohmann/json.hpp>

namespace indextools {

/**
 * @brief Cache of parsed source analyzers with a fan-out batch search engine.
 *
 * A CacheSystem maps each canonical file path to a parsed LangAnalyze plus the
 * file's size and last-modified time at parse time. On lookup, an entry is
 * considered valid only if the file's current size and mtime still match the
 * snapshot; otherwise it is treated as a miss and re-parsed. Analyzers are
 * minted by the process-wide LangPluginManager, which guarantees each analyzer
 * keeps its owning plugin library loaded for its whole lifetime.
 *
 * Instances must be owned by a std::shared_ptr (the class derives from
 * enable_shared_from_this): launch_search captures shared_from_this() into the
 * coroutines it spawns so the cache outlives every in-flight task.
 */
class CacheSystem: public std::enable_shared_from_this<CacheSystem> {
public:
    using AnalyzePtr = std::shared_ptr<LangAnalyze>;

private:
    /// One cached analyzer plus the file identity used for staleness checks.
    struct CacheEntry {
        AnalyzePtr content;                            ///< Parsed analyzer.
        std::filesystem::file_time_type last_modify;   ///< mtime at parse time.
        uintmax_t file_size;                           ///< size at parse time.
    };

    /// Number of concurrent search tasks launch_search fans out into (>= 1).
    const size_t _num_tasks;
    /// Executor every spawned search coroutine runs on.
    const boost::asio::any_io_executor _executor;
    /// Serializes all access to _cache so the map is effectively single-threaded.
    boost::asio::strand<boost::asio::any_io_executor> _cache_strand;
    /// Canonical path → cached analyzer entry.
    std::unordered_map<std::filesystem::path, CacheEntry> _cache;

public:
    /**
     * @brief Construct a cache bound to @p executor.
     *
     * @param num_tasks Maximum number of concurrent slices launch_search
     *                  divides its work into. Clamped to at least 1.
     * @param executor  The executor all search coroutines and the internal
     *                  cache strand run on.
     */
    CacheSystem(size_t num_tasks, boost::asio::any_io_executor executor);
    ~CacheSystem() = default;
    CacheSystem(const CacheSystem&) = delete;
    CacheSystem& operator = (const CacheSystem&) = delete;
    CacheSystem(CacheSystem&&) = delete;
    CacheSystem& operator = (CacheSystem&&) = delete;

private:
    /**
     * @brief Fetch a still-valid cached analyzer for @p absolute_path.
     *
     * Hops onto the cache strand, then returns the cached analyzer only if the
     * file's current size and mtime match the snapshot taken when it was
     * parsed. Returns nullptr on a miss, a stale entry, or any filesystem
     * error. Never throws.
     */
    boost::asio::awaitable<AnalyzePtr> _get_entry(const std::filesystem::path& absolute_path) const noexcept;

    /**
     * @brief Store (or overwrite) the cache entry for @p absolute_path.
     *
     * Hops onto the cache strand before mutating the map. Never throws.
     */
    boost::asio::awaitable<void> _write_entry(const std::filesystem::path& absolute_path, CacheEntry entry) noexcept;

public:
    /**
     * @brief Resolve @p paths to analyzers and apply @p payload to each,
     *        merging every per-file JSON result into one array.
     *
     * The input paths are first filtered to existing files, canonicalized,
     * sorted and deduplicated. The work is then split into up to _num_tasks
     * contiguous slices, each processed by its own spawned coroutine: for every
     * path a valid cached analyzer is reused, otherwise a plugin analyzer is
     * created, driven through open()/analyze(), and written back to the cache.
     * Files that no plugin claims are skipped.
     *
     * @p payload is any callable `nlohmann::json(const AnalyzePtr&)` — typically
     * a locate_* invocation. Its result is flattened into the aggregate when it
     * is a JSON array; a non-array, non-null result is appended as a single
     * element. Exceptions from any single file are swallowed so one bad file
     * cannot abort a slice or stall the collector.
     *
     * @tparam SearchPayload  Callable taking an AnalyzePtr and returning JSON.
     * @param paths    File paths to search (need not exist or be unique).
     * @param payload  Per-analyzer query to apply.
     * @return A JSON array aggregating every file's results (possibly empty).
     */
    template<typename SearchPayload>
    boost::asio::awaitable<nlohmann::json> launch_search(const std::vector<std::filesystem::path>& paths, SearchPayload&& payload) {
        auto normalized_paths_view = paths | std::views::filter([](const std::filesystem::path& path) { return std::filesystem::exists(path); })
                                           | std::views::transform([](const std::filesystem::path& path) { return std::filesystem::canonical(path); });
        std::vector<std::filesystem::path> normalized_paths(normalized_paths_view.begin(), normalized_paths_view.end());
        std::sort(normalized_paths.begin(), normalized_paths.end());
        normalized_paths.erase(std::unique(normalized_paths.begin(), normalized_paths.end()), normalized_paths.end());

        size_t task_running = 0;
        auto results = nlohmann::json::array();
        // concurrent_channel (not plain channel): the per-slice coroutines run
        // on the raw executor with no strand between them, so under a
        // multi-threaded io_context::run() several of them may call async_send
        // concurrently. Only concurrent_channel permits concurrent senders.
        auto result_channel = std::make_shared<boost::asio::experimental::concurrent_channel<void(boost::system::error_code, nlohmann::json)>>(_executor);

        size_t cnt_per_task = (normalized_paths.size() + _num_tasks - 1) / _num_tasks;
        for (size_t start_idx = 0; start_idx < normalized_paths.size(); start_idx += cnt_per_task) {
            task_running++;
            
            boost::asio::co_spawn(_executor, [
                &normalized_paths,
                &payload,
                result_channel,
                start_idx,
                end_idx = std::min(start_idx + cnt_per_task, normalized_paths.size()),
                self = shared_from_this()
            ]() -> boost::asio::awaitable<void> {
                auto split_results = nlohmann::json::array();

                for (size_t i = start_idx; i < end_idx; ++i) {
                    const auto& target_path = normalized_paths[i];

                    // Guard every file individually: a throw here must not abort
                    // the whole slice, otherwise this task would never reach the
                    // async_send below and the collector would hang forever.
                    try {
                        // Cache hit returns a ready-to-query analyzer; nullptr
                        // means either a miss or a stale entry (see _get_entry).
                        auto analyzer = co_await self->_get_entry(target_path);

                        if (analyzer == nullptr) {
                            // Pure extension routing — skip files no plugin claims.
                            if (!LangPluginManager::instance().is_supported(target_path)) {
                                continue;
                            }

                            analyzer = LangPluginManager::instance().make_lang_analyze(target_path);
                            if (analyzer == nullptr) {
                                continue;
                            }

                            // Load from disk and run language-specific analysis,
                            // then snapshot the file's identity so _get_entry can
                            // later detect staleness (size / mtime).
                            analyzer->open(target_path)->analyze();

                            CacheEntry entry;
                            entry.content = analyzer;
                            entry.last_modify = std::filesystem::last_write_time(target_path);
                            entry.file_size = std::filesystem::file_size(target_path);
                            co_await self->_write_entry(target_path, std::move(entry));
                        }

                        // Apply the caller's query (e.g. a locate_* call). The
                        // locate_* family returns a JSON array, so flatten it into
                        // the aggregate; tolerate a non-array/non-null payload by
                        // appending it as a single element.
                        nlohmann::json payload_result = payload(analyzer);
                        if (payload_result.is_array()) {
                            for (auto& element : payload_result) {
                                split_results.push_back(std::move(element));
                            }
                        } else if (!payload_result.is_null()) {
                            split_results.push_back(std::move(payload_result));
                        }
                    } catch (...) {
                        continue;
                    }
                }

                // Exactly one send per spawned task — the collector below performs
                // exactly `task_running` receives to match.
                co_await result_channel->async_send(
                    boost::system::error_code{}, std::move(split_results),
                    boost::asio::use_awaitable);
                co_return;
            }, boost::asio::detached);
        }

        // Drain one result batch per spawned task and merge into the aggregate.
        // launch_search stays suspended here until every task has sent, which
        // keeps the by-reference captures (normalized_paths, payload) alive.
        for (size_t i = 0; i < task_running; ++i) {
            nlohmann::json split_results = co_await result_channel->async_receive(boost::asio::use_awaitable);
            for (auto& element : split_results) {
                results.push_back(std::move(element));
            }
        }

        co_return results;
    }
};


/**
 * @brief High-level search facade keyed on (root directory, glob pattern).
 *
 * SearchInterface wraps a CacheSystem and shifts the unit of a query from an
 * explicit list of file paths to a directory tree described by a glob. Each
 * query expands the glob against the root via indextools::glob_find() to obtain
 * the concrete file set, then delegates to CacheSystem::launch_search with the
 * matching locate_* payload.
 *
 * It exposes the three query levels the analyzer layer speaks in:
 *   - locate_pattern    — substring or (opt-in) regex match across lines.
 *   - locate_identifier — occurrences of a pre-indexed identifier.
 *   - locate_entity     — a named entity (function/class/module/...) subtree.
 *
 * The wrapped CacheSystem may be supplied by the caller (to share one cache
 * across several facades) or created internally. Like CacheSystem, instances
 * are non-copyable; queries are coroutines that must be co_await-ed.
 */
class SearchInterface {
    /// The underlying analyzer cache + search engine. Never null.
    std::shared_ptr<CacheSystem> _cache;

public:
    /**
     * @brief Construct over a caller-provided cache (shared ownership).
     *
     * @param cache A non-null CacheSystem. Sharing one cache across facades
     *              lets independent queries reuse each other's parsed files.
     */
    explicit SearchInterface(std::shared_ptr<CacheSystem> cache): _cache(std::move(cache)) {}

    /**
     * @brief Construct with an internally-owned cache bound to @p executor.
     *
     * @param num_tasks Fan-out width forwarded to CacheSystem.
     * @param executor  Executor the internal cache and its searches run on.
     */
    SearchInterface(size_t num_tasks, boost::asio::any_io_executor executor)
        : _cache(std::make_shared<CacheSystem>(num_tasks, executor)) {}

    /// Access the underlying cache (e.g. to share it with another facade).
    const std::shared_ptr<CacheSystem>& cache() const noexcept { return _cache; }

    /**
     * @brief Locate all occurrences of @p pattern in files matched by the glob.
     *
     * Expands @p glob_pattern under @p root_path, then runs
     * LangAnalyze::locate_pattern(@p pattern, @p use_regex) on every matched
     * file and merges the results.
     *
     * @param root_path    Root directory the glob is resolved against.
     * @param glob_pattern Glob selecting the files to search (see glob_find()).
     * @param pattern      Substring or, when @p use_regex, ECMAScript regex.
     * @param use_regex    Treat @p pattern as a regular expression.
     * @return A JSON array of per-file match blocks (meta + text).
     */
    boost::asio::awaitable<nlohmann::json> locate_pattern(
        const std::filesystem::path& root_path,
        const std::string& glob_pattern,
        std::string pattern,
        bool use_regex = false);

    /**
     * @brief Locate occurrences of @p identifier in files matched by the glob.
     *
     * Expands @p glob_pattern under @p root_path, then runs
     * LangAnalyze::locate_identifier(@p identifier) on every matched file and
     * merges the results.
     *
     * @param root_path    Root directory the glob is resolved against.
     * @param glob_pattern Glob selecting the files to search.
     * @param identifier   Identifier to look up (as indexed during analysis).
     * @return A JSON array of per-file match blocks (meta + text).
     */
    boost::asio::awaitable<nlohmann::json> locate_identifier(
        const std::filesystem::path& root_path,
        const std::string& glob_pattern,
        std::string identifier);

    /**
     * @brief Locate the entity @p entity_key in files matched by the glob.
     *
     * Expands @p glob_pattern under @p root_path, then runs
     * LangAnalyze::locate_entity(@p entity_key) on every matched file and
     * merges the results. Each matching entity is returned with its full
     * descendant subtree.
     *
     * @param root_path    Root directory the glob is resolved against.
     * @param glob_pattern Glob selecting the files to search.
     * @param entity_key   Entity lookup key (e.g. a function/class name).
     * @return A JSON array of matching entity subtrees.
     */
    boost::asio::awaitable<nlohmann::json> locate_entity(
        const std::filesystem::path& root_path,
        const std::string& glob_pattern,
        std::string entity_key);
};


}
