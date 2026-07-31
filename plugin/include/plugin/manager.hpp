#pragma once

/**
 * @file plugin/manager.hpp
 * @brief Dynamic loading, ABI gating, and lifetime-safe instantiation.
 *
 * `PluginManager` is the *management* half of the framework: it knows how to
 * open a plugin `.so`/`.dll`, mint its descriptor via the exported factory,
 * verify the ABI, and — critically — build a `shared_ptr<Product>` whose deleter
 * keeps the owning library mapped for the object's entire lifetime.
 *
 * ## What is generalized
 *
 * The old `LangPluginManager` baked the language domain's factory symbol
 * (`create_lang_plugin`) and ABI version directly into its loader. Here both are
 * *parameters*: each domain passes its own factory name, ABI version, and a tiny
 * `Loader` that performs the typed `boost::dll::import_alias`. The same manager
 * loads language plugins, a test toy plugin, or any future domain.
 *
 * ## Lifetime binding (the crux of plugin mode)
 *
 * A `Product` produced by a dynamically-loaded plugin has its code and vtable
 * inside the plugin's library. If the library unloads while the product is still
 * alive, any call — including `~Product` — is undefined behavior. A bare
 * `shared_ptr<Product>` does NOT keep the library loaded.
 *
 * `create_instance()` solves this exactly as the old code did: it captures the
 * library's `shared_ptr` into the deleter of the returned `shared_ptr<Product>`,
 * so destruction order is guaranteed — the product is destroyed first (its dtor
 * code is still mapped), then the captured library reference drops. Callers use
 * ordinary `shared_ptr` RAII and never think about library unloading.
 *
 * ## Stateless
 *
 * The manager holds no state of its own; it is a bundle of load/instantiate
 * operations. The loaded set (plugins + their libraries) is owned by whatever
 * the domain stores them in — typically a `PluginDispatcher` payload plus a
 * `PluginRegistry`. `load()` therefore returns the library handle alongside the
 * plugin so the domain can attach it to its routing/registration state.
 */

#include <algorithm>
#include <cctype>
#include <concepts>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/dll/import.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/dll/shared_library.hpp>
#include <boost/dll/shared_library_load_mode.hpp>

#include "plugin/plugin.hpp"

namespace plugin {

/**
 * @brief Outcome of loading one plugin library.
 *
 * Carries the freshly minted plugin descriptor *and* the owning library handle,
 * so a domain can attach both to its routing/registration state in one step.
 * The library must outlive the plugin (and every product it creates); threading
 * it through here is what makes lifetime binding work without the manager having
 * to remember the association.
 *
 * `explicit operator bool` reports whether a plugin was actually loaded.
 */
template <typename ConcretePlugin>
struct LoadResult {
    std::shared_ptr<ConcretePlugin> plugin;
    std::shared_ptr<boost::dll::shared_library> library;

    explicit operator bool() const noexcept { return static_cast<bool>(plugin); }
};

namespace detail {

/// True if @p path has a platform dynamic-library extension (.so / .dylib / .dll).
inline bool is_dynamic_library(const std::filesystem::path& path) noexcept {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#if defined(_WIN32)
    return ext == ".dll";
#elif defined(__APPLE__)
    return ext == ".dylib" || ext == ".so";
#else
    return ext == ".so";
#endif
}

} // namespace detail

/**
 * @brief Loads plugins of product type @p Product and binds their lifetimes.
 *
 * Stateless: see the file header. Compose with `PluginDispatcher` and
 * `PluginRegistry` to hold the loaded set.
 */
template <PluginProduct Product>
class PluginManager {
public:
    /**
     * @brief One loaded plugin: its owning library and its descriptor.
     *
     * Declaration order is load-bearing: `library` is declared FIRST so it is
     * destroyed LAST. A plugin object (and every product it creates) has its
     * vtable and destructor code inside the library, so the library must stay
     * mapped until after the plugin descriptor is destroyed. Keeping the loaded
     * set here — in the manager, which composes hosts destroy first — is what
     * makes teardown safe regardless of how a domain's router/registry order
     * their own destruction.
     */
    struct Loaded {
        std::shared_ptr<boost::dll::shared_library> library;
        std::shared_ptr<Plugin<Product>> plugin;
    };

    /// Number of libraries currently loaded (kept alive by this manager).
    std::size_t loaded_count() const noexcept { return _loaded.size(); }

    /**
     * @brief Load a single plugin library and verify it.
     *
     * Opens the library (with `append_decorations`, so a bare stem or a full
     * "libfoo.so" both work), checks for the factory symbol, invokes @p loader
     * to mint the descriptor, and checks the ABI version. On any failure the
     * library is discarded and an empty result is returned; a bad plugin never
     * affects already-loaded ones.
     *
     * @tparam ConcretePlugin The domain's plugin type (derives from
     *                        `Plugin<Product>`); the returned descriptor is
     *                        typed as this so the domain reads its own metadata
     *                        with no downcast.
     * @tparam Loader         Callable `shared_ptr<ConcretePlugin>(
     *                        boost::dll::shared_library&)` that performs the
     *                        `import_alias<DomainFactory>` + invoke. It captures
     *                        the domain's factory name and factory type.
     * @param lib_path      Path to the plugin.
     * @param factory_name  The BOOST_DLL_ALIAS name to import.
     * @param abi_version   Expected ABI version; mismatched plugins are skipped.
     * @param loader        The factory-importing callable.
     * @return The loaded plugin + its library, or an empty result on failure.
     */
    template <typename ConcretePlugin, typename Loader>
        requires std::derived_from<ConcretePlugin, Plugin<Product>>
    LoadResult<ConcretePlugin> load(const std::filesystem::path& lib_path,
                                    std::string_view factory_name,
                                    std::uint32_t abi_version,
                                    Loader&& loader) {
        namespace dll = boost::dll;

        std::shared_ptr<dll::shared_library> library;
        std::shared_ptr<ConcretePlugin> plugin;
        try {
            library = std::make_shared<dll::shared_library>(
                lib_path, dll::load_mode::append_decorations);

            if (!library->has(std::string(factory_name))) {
                std::cerr << "[plugin] skip " << lib_path
                          << ": missing symbol '" << factory_name << "'\n";
                return {};
            }

            // The loader performs the typed import_alias + invoke. The returned
            // descriptor (living inside the library) is kept alive by `library`.
            plugin = loader(*library);
        } catch (const std::exception& e) {
            std::cerr << "[plugin] failed to load " << lib_path
                      << ": " << e.what() << '\n';
            return {};
        }

        if (!plugin) {
            std::cerr << "[plugin] " << lib_path
                      << ": factory returned nullptr\n";
            return {};
        }
        if (plugin->abi_version() != abi_version) {
            std::cerr << "[plugin] " << lib_path << " (" << plugin->name()
                      << "): ABI version mismatch — plugin "
                      << plugin->abi_version() << ", host " << abi_version
                      << "; skipped\n";
            return {};
        }

        // Keep the library + descriptor alive in the manager with the correct
        // destruction order (library-first). Copies: the LoadResult carries its
        // own refs for the caller to wire into routing/registration state.
        _loaded.push_back(Loaded{library, plugin});
        return LoadResult<ConcretePlugin>{std::move(plugin), std::move(library)};
    }

    /**
     * @brief Load every plugin found directly inside @p directory.
     *
     * Scans for regular files with a platform dynamic-library extension and
     * calls `load()` on each. For each plugin that loads successfully, @p on_loaded
     * is invoked with the result so the domain can wire it into its router and
     * registry. Files that fail to load are logged and skipped — one bad plugin
     * does not abort discovery.
     *
     * @return The number of plugins successfully loaded.
     */
    template <typename ConcretePlugin, typename Loader, typename OnLoaded>
        requires std::derived_from<ConcretePlugin, Plugin<Product>>
    std::size_t load_directory(const std::filesystem::path& directory,
                               std::string_view factory_name,
                               std::uint32_t abi_version,
                               Loader&& loader, OnLoaded&& on_loaded) {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            std::cerr << "[plugin] not a directory: " << directory << '\n';
            return 0;
        }

        std::size_t loaded = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec) ||
                !detail::is_dynamic_library(entry.path())) {
                continue;
            }
            auto result = load<ConcretePlugin>(entry.path(), factory_name,
                                               abi_version, loader);
            if (result) {
                on_loaded(result);
                ++loaded;
            }
        }
        return loaded;
    }

    /**
     * @brief Convenience: load plugins from `<executable_dir>/plugins`.
     *
     * Uses `boost::dll::program_location()` so it works regardless of the
     * current working directory. The `.string()` round-trip bridges whichever
     * filesystem flavor Boost.DLL was built with back to std::filesystem::path
     * (robust even if `BOOST_DLL_USE_STD_FS` is not defined).
     *
     * @return The number of plugins successfully loaded.
     */
    template <typename ConcretePlugin, typename Loader, typename OnLoaded>
        requires std::derived_from<ConcretePlugin, Plugin<Product>>
    std::size_t load_default_directory(std::string_view factory_name,
                                       std::uint32_t abi_version,
                                       Loader&& loader, OnLoaded&& on_loaded) {
        namespace dll = boost::dll;
        std::error_code ec;
        auto exe = dll::program_location(ec);
        if (ec) {
            std::cerr << "[plugin] cannot resolve program location: "
                      << ec.message() << '\n';
            return 0;
        }
        std::filesystem::path exe_path(exe.string());
        return load_directory<ConcretePlugin>(exe_path.parent_path() / "plugins",
                                              factory_name, abi_version,
                                              loader, on_loaded);
    }

    /**
     * @brief Build a lifetime-safe `shared_ptr<Product>` from a plugin.
     *
     * Calls `plugin->create()`, then wraps the result in a `shared_ptr` whose
     * deleter captures @p library. The product is destroyed first (its dtor code,
     * in the plugin, is still mapped), then the library reference drops — so the
     * library outlives every product it created. Pass the library that owns
     * @p plugin (the same handle attached to the dispatcher entry).
     *
     * @return The instance, or nullptr if @p plugin is null or `create()` fails.
     */
    std::shared_ptr<Product> create_instance(
        std::shared_ptr<Plugin<Product>> plugin,
        std::shared_ptr<boost::dll::shared_library> library) const {
        if (!plugin) {
            return nullptr;
        }
        std::unique_ptr<Product> raw = plugin->create();
        if (!raw) {
            return nullptr;
        }
        return std::shared_ptr<Product>(
            raw.release(),
            [lib = std::move(library)](Product* p) noexcept {
                delete p;
                // `lib` drops here, after the object is fully destroyed.
            });
    }

private:
    // Canonical owner of every loaded library + descriptor, with library-first
    // destruction ordering (see Loaded). A domain's router/registry also hold
    // shared_ptr refs for routing/lookup; this vector is what guarantees safe
    // teardown.
    std::vector<Loaded> _loaded;
};

} // namespace plugin
