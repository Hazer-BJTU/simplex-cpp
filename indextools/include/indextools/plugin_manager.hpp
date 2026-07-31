#pragma once

/**
 * @file plugin_manager.hpp
 * @brief Runtime loader and registry for language plugins.
 *
 * `LangPluginManager` is the language *domain* layer: it composes the generic
 * `plugin::` framework (PluginManager / PluginDispatcher / PluginRegistry,
 * parameterized by `LangAnalyze`) and adds the one piece of routing logic that
 * is specific to languages — compiling each plugin's `file_pattern()` regex and
 * matching file names against it.
 *
 * It preserves the public surface the rest of the codebase already expects:
 *   - is_supported(path)
 *   - make_lang_analyze(path)  ->  std::shared_ptr<LangAnalyze>
 *
 * ## Composition
 *
 *   - plugin::PluginManager<LangAnalyze>   _loader   — opens .so, ABI gate,
 *     lifetime-safe instantiation (library bound into the analyzer's deleter).
 *   - plugin::PluginDispatcher<...>        _router   — priority-ordered,
 *     regex-based file→plugin routing (the LangFileSelector policy).
 *   - plugin::PluginRegistry<LangAnalyze>  _registry — by-name lookup.
 *
 * All three are populated together in _load() (load + regex compile + register
 * is atomic, so a plugin with a bad pattern is rejected outright and never
 * counted or routed).
 *
 * ## Routing: regex patterns, ordered by priority
 *
 * Each plugin exposes a single regex (LangPlugin::file_pattern()) matched
 * against a file's *name*, so fixed-name/extensionless files (Dockerfile,
 * CMakeLists.txt) route correctly. Plugins are ordered by priority() (highest
 * first, load order breaking ties); routing walks that order and picks the
 * first plugin whose pattern matches. A low-priority catch-all is thus
 * overridden by any dedicated plugin without touching the catch-all's pattern.
 *
 * ## Lifetime safety (the crux of plugin mode)
 *
 * A LangAnalyze produced by a plugin has its code and vtable inside the
 * plugin's dynamic library. If the library unloads while an analyzer is still
 * alive, any call (including ~LangAnalyze) is undefined behavior. A bare
 * shared_ptr<LangAnalyze> does NOT keep the library loaded.
 *
 * The generic loader solves this by binding a shared_ptr to the plugin's
 * boost::dll::shared_library into the deleter of every analyzer shared_ptr it
 * returns (the library handle is carried alongside the plugin in the router
 * payload). Destruction order is then guaranteed: the analyzer is destroyed
 * first (its dtor code is still mapped), then the captured library reference
 * drops. The library only truly unloads once the manager and every analyzer it
 * ever produced are gone.
 *
 * ## Singleton
 *
 * There is exactly one manager per process, reached through instance(). Since
 * the query surface is const and safe under concurrent readers, the only
 * mutation — loading plugins — is funnelled through ensure_loaded(), which uses
 * std::call_once so plugins are discovered exactly once no matter how many
 * threads race to trigger it. After the one-time load _router.flush() freezes
 * the routing order, so every subsequent const query is a pure read (and
 * is_supported stays noexcept).
 */

#include <filesystem>
#include <memory>
#include <mutex>
#include <regex>
#include <string>

#include <boost/dll/shared_library.hpp>

#include "indextools/lang.hpp"
#include "indextools/lang_plugin.hpp"
#include "plugin/dispatcher.hpp"
#include "plugin/manager.hpp"
#include "plugin/registry.hpp"

namespace indextools {

/**
 * @brief Process-wide loader that routes files to the right analyzer.
 *
 * Access the sole instance via instance(). Trigger discovery once with
 * ensure_loaded() (idempotent, thread-safe via std::call_once); afterwards the
 * manager is read-only and its const query methods are safe to call
 * concurrently.
 */
class LangPluginManager {
public:
    using AnalyzePtr = std::shared_ptr<LangAnalyze>;

    /**
     * @brief The one manager for this process.
     *
     * Constructed on first call (thread-safe static-local init). Destroyed at
     * process exit; analyzers already handed out stay valid because each holds
     * its own library reference through its deleter.
     */
    static LangPluginManager& instance();

    /**
     * @brief Load plugins exactly once, from `<executable_dir>/plugins`.
     *
     * The first caller performs discovery; concurrent and subsequent callers
     * block until it finishes, then return without reloading. Safe to call from
     * any thread and as many times as you like.
     *
     * @return The number of plugins loaded by the one-time discovery.
     */
    std::size_t ensure_loaded();

    /**
     * @brief Load plugins exactly once, from an explicit @p directory.
     *
     * Same one-shot semantics as ensure_loaded(); the directory of the first
     * winning call is the one used. Intended for tests and non-default layouts.
     */
    std::size_t ensure_loaded(const std::filesystem::path& directory);

private:
    // ---- domain routing policy (regex over the file name) -------------------
    // Carried per-plugin in the dispatcher payload: the compiled matcher plus
    // the owning library handle (so make_lang_analyze can bind the library into
    // the analyzer's deleter without searching).
    struct LangFilePayload {
        std::regex pattern;
        std::shared_ptr<boost::dll::shared_library> library;
    };

    // Selector policy for PluginDispatcher: true when the payload's regex
    // matches the routed file's *name* (not its directory path).
    struct LangFileSelector {
        bool operator()(const LangFilePayload& payload,
                        const std::filesystem::path& file_path) const {
            return std::regex_search(file_path.filename().string(), payload.pattern);
        }
    };

    using Router = plugin::PluginDispatcher<LangAnalyze, std::filesystem::path,
                                            LangFileSelector, LangFilePayload>;

    plugin::PluginManager<LangAnalyze> _loader;
    Router _router;
    plugin::PluginRegistry<LangAnalyze> _registry;
    std::once_flag _load_once;

    // A singleton: constructed only by instance(), never copied or moved.
    LangPluginManager() = default;
    ~LangPluginManager() = default;
    LangPluginManager(const LangPluginManager&) = delete;
    LangPluginManager& operator=(const LangPluginManager&) = delete;
    LangPluginManager(LangPluginManager&&) = delete;
    LangPluginManager& operator=(LangPluginManager&&) = delete;

    /**
     * @brief Mint the plugin descriptor from an opened library.
     *
     * The domain-specific step the generic loader cannot do itself: import the
     * `LangPluginFactory` alias and invoke it. The returned descriptor lives
     * inside @p library; the loader keeps the library alive.
     */
    static std::shared_ptr<LangPlugin> _mint_plugin(boost::dll::shared_library& library);

    /**
     * @brief Load a single plugin library, compile its routing regex, register.
     *
     * Delegates the .so open / factory import / ABI check to the generic loader,
     * then compiles file_pattern() host-side (rejecting the plugin on a bad
     * regex) and registers the plugin into the router and registry atomically.
     *
     * @return true if the plugin loaded and registered; false on any failure
     *         (already-loaded plugins are unaffected).
     */
    bool _load(const std::filesystem::path& library_path);

    /**
     * @brief Load every plugin found directly inside @p directory.
     *
     * Scans for regular files with a platform dynamic-library extension and
     * calls _load() on each, then freezes the routing order. Files that fail to
     * load are logged and skipped — one bad plugin does not abort discovery.
     *
     * @return The number of plugins successfully loaded.
     */
    std::size_t _load_directory(const std::filesystem::path& directory);

    /**
     * @brief Convenience: load plugins from `<executable_dir>/plugins`.
     *
     * Uses boost::dll::program_location() so it works regardless of the current
     * working directory. This is the default the host uses at startup.
     *
     * @return The number of plugins successfully loaded.
     */
    std::size_t _load_default_directory();

public:
    // ------------------------------------------------------------------ query

    /// Number of loaded (routed) plugins.
    std::size_t plugin_count() const noexcept { return _router.size(); }

    /**
     * @brief Whether some loaded plugin's pattern matches @p file_path's name.
     *
     * Case-insensitive regex match against the file name (not the full path).
     * Unlike the old generated helper, this does NOT check file existence —
     * routing is a pure name decision; existence is the caller's concern (see
     * CacheSystem, which canonicalizes).
     */
    bool is_supported(const std::filesystem::path& file_path) const noexcept;

    /**
     * @brief Create an analyzer for @p file_path, or nullptr if unsupported.
     *
     * The returned shared_ptr owns the analyzer and, via its deleter, a
     * reference to the plugin's library — guaranteeing the library outlives
     * the analyzer. The caller then drives open()/load()/analyze() as before.
     */
    AnalyzePtr make_lang_analyze(const std::filesystem::path& file_path) const;
};

}
