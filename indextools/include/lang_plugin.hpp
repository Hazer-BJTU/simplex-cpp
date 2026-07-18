#pragma once

/**
 * @file lang_plugin.hpp
 * @brief Shared plugin ABI between the indextools host and language plugins.
 *
 * This header is the *contract* — it is compiled into both the host executable
 * and every language plugin (`.so`/`.dll`). It defines:
 *
 *   - LangPlugin      : the abstract interface a plugin exposes to the host.
 *   - LANG_PLUGIN_ABI_VERSION : a monotonically increasing integer bumped
 *                       whenever this interface or LangAnalyze changes in a
 *                       binary-incompatible way. The host refuses to load a
 *                       plugin whose abi_version() does not match.
 *   - LANG_PLUGIN_EXPORT : cross-platform symbol-visibility macro.
 *
 * ## Why a separate LangPlugin from LangAnalyze?
 *
 * LangAnalyze is a *per-file* analyzer object with lots of state. A plugin,
 * by contrast, is a long-lived, stateless descriptor of a language: it knows
 * the language name, the file extensions it handles, and how to mint fresh
 * LangAnalyze instances. Splitting them keeps the plugin object tiny and lets
 * the host query metadata (name/extensions) without constructing an analyzer.
 *
 * ## Lifetime contract (important)
 *
 * A plugin object and every LangAnalyze it creates live *inside* the loaded
 * dynamic library. Their vtables, code, and (for `create()`) their allocating
 * `operator new` all belong to the plugin module. Therefore:
 *
 *   - The plugin's `boost::dll::shared_library` MUST outlive the LangPlugin
 *     object AND every LangAnalyze it produced.
 *   - Objects allocated by the plugin MUST be destroyed while the library is
 *     still loaded (destructor code lives in the plugin).
 *
 * LangPluginManager enforces this by binding the library's lifetime into the
 * deleter of the shared_ptr<LangAnalyze> it hands out (see plugin_manager.hpp).
 * Because both the host and the plugins are built with the same toolchain, a
 * C++ virtual interface across the boundary is safe.
 *
 * ## Authoring a plugin
 *
 * A plugin translation unit implements a LangPlugin subclass, provides a
 * factory that returns `std::shared_ptr<indextools::LangPlugin>`, and exports
 * it under the well-known alias name via BOOST_DLL_ALIAS:
 *
 * @code
 *   std::shared_ptr<indextools::LangPlugin> create_lang_plugin() {
 *       return std::make_shared<MyPlugin>();
 *   }
 *   BOOST_DLL_ALIAS(create_lang_plugin, create_lang_plugin)
 * @endcode
 *
 * The host imports the alias `create_lang_plugin` (see LANG_PLUGIN_FACTORY_NAME).
 *
 * ## File matching (regex, not extension tables)
 *
 * A plugin no longer enumerates extensions. Instead it exposes a single regular
 * expression via file_pattern() that matches every file name the language
 * claims. The host matches this pattern (case-insensitively, via regex_search)
 * against a file's *name* — not its full path — so a language can claim
 * extensionless or fixed-name files like "Dockerfile" or "CMakeLists.txt", not
 * just ".ext" suffixes.
 *
 * When several loaded plugins match the same file, the host picks the one with
 * the highest priority() (ties broken by load order). A broad catch-all plugin
 * therefore returns a low priority so that a dedicated language plugin, added
 * later, automatically wins without anyone editing the catch-all's pattern.
 */

#include <memory>
#include <string_view>
#include <cstdint>

#include "lang.hpp"
#include "versioning/version.hpp"

#if defined(_WIN32)
    #define LANG_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
    #define LANG_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace indextools {

/// Bump whenever LangPlugin or LangAnalyze changes in a binary-incompatible
/// way. The host rejects any plugin reporting a different value.
///
/// v2: replaced extensions() (extension table) with file_pattern() (regex on
///     the file name) and added priority() for match ordering.
///
/// The canonical value lives in the sibling versioning/ module
/// (build-generated versioning/version.hpp → simplex::LANG_PLUGIN_ABI_VERSION).
/// This alias keeps the existing indextools-namespace references unchanged.
inline constexpr std::uint32_t LANG_PLUGIN_ABI_VERSION = simplex::LANG_PLUGIN_ABI_VERSION;

/// Default priority for plugins that do not override priority(). Higher wins;
/// a broad catch-all should return a value below this so real language plugins
/// override it. See LangPlugin::priority().
inline constexpr int LANG_PLUGIN_DEFAULT_PRIORITY = 0;

/// Conventional priority for a broad catch-all/fallback plugin: low enough that
/// any dedicated language plugin (using the default) outranks it.
inline constexpr int LANG_PLUGIN_FALLBACK_PRIORITY = -1000;

/// The BOOST_DLL_ALIAS name the host looks up in every plugin.
inline constexpr const char* LANG_PLUGIN_FACTORY_NAME = "create_lang_plugin";

/**
 * @brief Abstract descriptor + factory for one language.
 *
 * Instances are created by the plugin's exported factory and owned by the
 * host's LangPluginManager. A plugin is stateless with respect to individual
 * files; it only knows which extensions it serves and how to build analyzers.
 */
class LangPlugin {
public:
    virtual ~LangPlugin() = default;

    /**
     * @brief ABI version this plugin was built against.
     *
     * Must equal LANG_PLUGIN_ABI_VERSION for the host to accept the plugin.
     * Implementations should simply `return LANG_PLUGIN_ABI_VERSION;`.
     */
    virtual std::uint32_t abi_version() const noexcept = 0;

    /// Human-readable language name (e.g. "Python", "Fallback"). Used for
    /// logging and diagnostics; not required to be unique but should be.
    virtual std::string_view name() const noexcept = 0;

    /**
     * @brief ECMAScript regular expression matching every file name this
     *        language claims.
     *
     * The host matches this pattern against a file's *name* (e.g. "main.py",
     * "Dockerfile", "CMakeLists.txt"), not its directory path, using a
     * case-insensitive regex_search — so the pattern need not be anchored, but
     * anchoring (e.g. `\.py$`) is recommended to avoid accidental substring
     * hits. Example for Python: `\.(py|pyw|pyi)$`.
     *
     * Returning an empty string is legal but means the plugin never matches by
     * name. The pattern is compiled once at load; an invalid regex causes the
     * plugin to be rejected.
     */
    virtual std::string_view file_pattern() const noexcept = 0;

    /**
     * @brief Match priority when several plugins claim the same file.
     *
     * The host evaluates loaded plugins in priority order (highest first, ties
     * broken by load order) and selects the first whose file_pattern() matches.
     * A dedicated language plugin should keep the default
     * (LANG_PLUGIN_DEFAULT_PRIORITY); a broad catch-all should return a low
     * value (e.g. LANG_PLUGIN_FALLBACK_PRIORITY) so it is only chosen when no
     * dedicated plugin matches. Overriding is optional.
     */
    virtual int priority() const noexcept { return LANG_PLUGIN_DEFAULT_PRIORITY; }

    /**
     * @brief Mint a fresh, empty analyzer for this language.
     *
     * The returned object is allocated inside the plugin module. The caller
     * (LangPluginManager) is responsible for ensuring the owning library
     * outlives the returned analyzer — it does so via a custom deleter.
     *
     * @return A newly constructed LangAnalyze, never null on success.
     */
    virtual std::unique_ptr<LangAnalyze> create() const = 0;
};

/// Signature of the exported factory alias every plugin must provide.
using LangPluginFactory = std::shared_ptr<LangPlugin>();

}
