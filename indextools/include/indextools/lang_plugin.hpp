#pragma once

/**
 * @file lang_plugin.hpp
 * @brief Shared plugin ABI between the indextools host and language plugins.
 *
 * This header is the *contract* — it is compiled into both the host executable
 * and every language plugin (`.so`/`.dll`). It defines:
 *
 *   - LangPlugin      : the abstract descriptor a plugin exposes to the host.
 *   - LANG_PLUGIN_ABI_VERSION     : bumped whenever this interface or LangAnalyze
 *                       changes in a binary-incompatible way. The host's
 *                       LangDispatcher rejects any plugin whose abi_version()
 *                       does not match.
 *   - LANG_PLUGIN_FACTORY_NAME    : the descriptor-factory alias ("create_lang_plugin").
 *   - LANG_ANALYZE_FACTORY_NAME   : the analyzer-factory alias ("create_lang_analyze").
 *
 * ## Two exported aliases per plugin
 *
 * A plugin `.so` is loaded through the generic `extension_framework`. It exports
 * TWO factory aliases:
 *
 *   - `create_lang_plugin`  -> std::unique_ptr<extension::ExtensionContext>
 *       Mints the long-lived, stateless LangPlugin descriptor (identity + the
 *       file-name regex it claims). Returned as the base ExtensionContext so the
 *       generic loader needs no language-specific type.
 *
 *   - `create_lang_analyze` -> std::unique_ptr<LangAnalyze>
 *       Mints a fresh per-file analyzer. Resolved ONCE per plugin into a cached
 *       product_factory (see LangPlugin::warm), so the parallel analyzer-creation
 *       path in CacheSystem pays only a single indirect call per analyzer.
 *
 * @code
 *   std::unique_ptr<extension::ExtensionContext> create_lang_plugin() {
 *       return std::make_unique<MyPlugin>();
 *   }
 *   std::unique_ptr<LangAnalyze> create_lang_analyze() {
 *       return std::make_unique<MyAnalyzer>();
 *   }
 *   BOOST_DLL_ALIAS(create_lang_plugin, create_lang_plugin)
 *   BOOST_DLL_ALIAS(create_lang_analyze, create_lang_analyze)
 * @endcode
 *
 * ## Why a separate LangPlugin from LangAnalyze?
 *
 * LangAnalyze is a *per-file* analyzer object with lots of state. The plugin
 * descriptor, by contrast, is a long-lived, stateless descriptor of a language:
 * it knows the language name, the file-name regex it claims, and how to mint
 * fresh LangAnalyze instances (via its warmed product_factory). Splitting them
 * keeps the descriptor tiny and lets the host query metadata without constructing
 * an analyzer.
 *
 * ## Lifetime
 *
 * Both the descriptor and every analyzer it creates live inside the loaded
 * dynamic library (vtable + destructor + allocating `operator new` all belong to
 * the plugin module). The descriptor is bound to its library by the loader
 * (ExtensionContext::bind). Analyzers are minted through a product_factory in
 * its default deleter-pinned mode, so each analyzer shared_ptr carries its own
 * library reference and is safely destructible independently — no process-wide
 * singleton is needed to keep libraries mapped.
 *
 * ## File matching (regex on the file name)
 *
 * A plugin exposes one ECMAScript regex via file_pattern() matched
 * case-insensitively against a file's *name* (not its full path). The host
 * compiles it once (LangPlugin::warm) and routes by priority: when several
 * plugins match, the highest-priority one wins (ties broken by load order).
 */

#include <cstdint>
#include <filesystem>
#include <memory>
#include <regex>
#include <string>
#include <string_view>

#include "indextools/lang.hpp"
#include "extension_framework/extensions.hpp"
#include "versioning/version.hpp"

namespace indextools {

/// Bump whenever LangPlugin or LangAnalyze changes in a binary-incompatible way.
/// The host's LangDispatcher rejects any plugin reporting a different value.
///
/// v4: LangPlugin now derives from extension::ExtensionContext (not
///     plugin::Plugin<LangAnalyze>); analyzer creation moved from the virtual
///     Plugin::create() to the exported `create_lang_analyze` alias, cached in a
///     product_factory. The descriptor factory now returns unique_ptr<ExtensionContext>.
///
/// The canonical value lives in the sibling versioning/ module (build-generated
/// versioning/version.hpp -> simplex::LANG_PLUGIN_ABI_VERSION). This alias keeps
/// the existing indextools-namespace references unchanged.
inline constexpr std::uint32_t LANG_PLUGIN_ABI_VERSION = simplex::LANG_PLUGIN_ABI_VERSION;

/// Default priority for plugins that do not override priority(). Higher wins;
/// a broad catch-all should return a value below this so real language plugins
/// override it.
inline constexpr int LANG_PLUGIN_DEFAULT_PRIORITY = 0;

/// Conventional priority for a broad catch-all/fallback plugin: low enough that
/// any dedicated language plugin (using the default) outranks it.
inline constexpr int LANG_PLUGIN_FALLBACK_PRIORITY = -1000;

/// The BOOST_DLL_ALIAS name the host imports to mint the descriptor.
inline constexpr const char* LANG_PLUGIN_FACTORY_NAME = "create_lang_plugin";

/// The BOOST_DLL_ALIAS name the host imports (cached once per plugin) to mint
/// fresh LangAnalyze instances.
inline constexpr const char* LANG_ANALYZE_FACTORY_NAME = "create_lang_analyze";

/**
 * @brief Abstract descriptor + analyzer factory for one language.
 *
 * Specializes the generic `extension::ExtensionContext` base: the language
 * domain inherits `abi_version()`, `name()`, and `priority()` from it and adds
 * one piece of domain-specific routing metadata, `file_pattern()`. Instances are
 * produced by the plugin's exported `create_lang_plugin` factory and owned by the
 * host's LangDispatcher.
 *
 * After load, the host calls warm() once (single-threaded) to compile the
 * file-name regex and resolve the cached product_factory<LangAnalyze>. From then
 * on matches() and create_analyzer() are pure read-only operations, safe to call
 * concurrently — which is what CacheSystem::launch_search does when it fans out
 * parallel analyzer creation across its task coroutines.
 */
class LangPlugin : public extension::ExtensionContext {
public:
    /**
     * @brief ECMAScript regular expression matching every file name this
     *        language claims.
     *
     * Matched case-insensitively against a file's *name* (e.g. "main.py",
     * "Dockerfile"), not its directory path, so the pattern need not be anchored
     * — but anchoring (e.g. `\.py$`) is recommended to avoid accidental substring
     * hits. Example for Python: `\.py[wi]?$`.
     *
     * Compiled once in warm(); an invalid regex causes warm() to fail and the
     * plugin is then inert (matches nothing).
     */
    virtual std::string_view file_pattern() const noexcept = 0;

    /**
     * @brief Prepare routing + creation state. Call exactly once, after the
     *        descriptor is bound to its library (single-threaded, at load time).
     *
     * Compiles file_pattern() into a case-insensitive regex and builds a cached
     * product_factory<LangAnalyze> from the bound library + LANG_ANALYZE_FACTORY_NAME.
     * Idempotent: a second call is a no-op. After load this state is immutable, so
     * matches()/create_analyzer() need no synchronization.
     *
     * @return true if both the regex compiled and the analyzer factory resolved;
     *         false otherwise (the plugin then matches nothing and is effectively
     *         rejected).
     */
    bool warm() noexcept {
        if (_warmed) {
            return _valid;
        }
        _warmed = true;
        _valid = false;

        try {
            _pattern = std::regex(
                std::string(file_pattern()),
                std::regex::ECMAScript | std::regex::icase);
        } catch (const std::regex_error&) {
            return false;
        }

        try {
            auto library_ref = get_library_ref();
            if (!library_ref) {
                return false;
            }
            _factory = std::make_shared<extension::product_factory<LangAnalyze>>(
                library_ref, LANG_ANALYZE_FACTORY_NAME);
        } catch (const std::exception&) {
            _factory.reset();
            return false;
        }

        _valid = true;
        return true;
    }

    /**
     * @brief Whether this plugin claims @p filename (case-insensitive regex match).
     *
     * @p filename should be the file's name, not its full path. Returns false if
     * warm() was never called or failed.
     */
    bool matches(std::string_view filename) const noexcept {
        if (!_valid) {
            return false;
        }
        try {
            return std::regex_search(std::string(filename), _pattern);
        } catch (...) {
            return false;
        }
    }

    /**
     * @brief Mint a fresh analyzer via the cached product_factory.
     *
     * The returned shared_ptr is deleter-pinned (product_factory's default): it
     * carries its own library reference, so the analyzer is safely destructible
     * independently of the dispatcher/descriptor. Returns nullptr if warm() did
     * not succeed or creation failed.
     */
    std::shared_ptr<LangAnalyze> create_analyzer() const noexcept {
        if (!_valid || !_factory) {
            return nullptr;
        }
        try {
            return _factory->create();
        } catch (const std::exception&) {
            return nullptr;
        }
    }

private:
    /// Compiled file-name matcher (set by warm()).
    std::regex _pattern;
    /// Cached analyzer factory (set by warm()); null until warmed/failed.
    std::shared_ptr<extension::product_factory<LangAnalyze>> _factory;
    /// Guards against re-warming.
    bool _warmed = false;
    /// Whether warm() produced a usable regex + factory.
    bool _valid = false;
};

}
