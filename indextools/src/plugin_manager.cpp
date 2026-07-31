#include "indextools/plugin_manager.hpp"

#include <iostream>
#include <system_error>
#include <utility>

#include <boost/dll/import.hpp>
#include <boost/dll/runtime_symbol_info.hpp>

namespace indextools {

LangPluginManager& LangPluginManager::instance() {
    // Function-local static: thread-safe one-time construction (C++11 [stmt.dcl]).
    static LangPluginManager manager;
    return manager;
}

std::shared_ptr<LangPlugin>
LangPluginManager::_mint_plugin(boost::dll::shared_library& library) {
    // import_alias returns a callable that itself holds a library reference;
    // we invoke it once to mint the descriptor, then drop it. The descriptor
    // (living inside the library) is kept alive by the loader holding `library`.
    auto factory = boost::dll::import_alias<LangPluginFactory>(
        library, LANG_PLUGIN_FACTORY_NAME);
    return factory();
}

std::size_t LangPluginManager::ensure_loaded() {
    std::call_once(_load_once, [this] { _load_default_directory(); });
    return _router.size();
}

std::size_t LangPluginManager::ensure_loaded(const std::filesystem::path& directory) {
    std::call_once(_load_once, [this, &directory] { _load_directory(directory); });
    return _router.size();
}

bool LangPluginManager::_load(const std::filesystem::path& library_path) {
    // Delegate the .so open / factory import / ABI check to the generic loader.
    // _mint_plugin is the domain-specific factory import (typed on LangPlugin).
    auto result = _loader.load<LangPlugin>(library_path, LANG_PLUGIN_FACTORY_NAME,
                                           LANG_PLUGIN_ABI_VERSION,
                                           &LangPluginManager::_mint_plugin);
    if (!result) {
        return false;
    }

    // Compile the file-name matcher host-side. A bad pattern rejects the whole
    // plugin: silently loading an analyzer that can never be routed to would be
    // more confusing than a clear diagnostic.
    std::regex pattern;
    try {
        pattern = std::regex(std::string(result.plugin->file_pattern()),
                             std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error& e) {
        std::cerr << "[plugin] " << library_path << " (" << result.plugin->name()
                  << "): invalid file_pattern() '" << result.plugin->file_pattern()
                  << "': " << e.what() << "; skipped\n";
        return false;
    }

    // Register atomically: the routing regex and the owning library travel
    // together as the dispatcher payload, and the plugin is also indexed by
    // name. A plugin reaches the router only if every step above succeeded.
    LangFilePayload payload{std::move(pattern), result.library};
    _router.add(result.plugin, std::move(payload));
    _registry.register_plugin(result.plugin);
    return true;
}

std::size_t LangPluginManager::_load_directory(const std::filesystem::path& directory) {
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
            !plugin::detail::is_dynamic_library(entry.path())) {
            continue;
        }
        if (_load(entry.path())) {
            ++loaded;
        }
    }
    // Directory iteration order is unspecified, so priority — not filesystem
    // order — must decide routing. Freeze the priority order now so the const
    // query surface (notably is_supported, which is noexcept) never has to sort.
    _router.flush();
    return loaded;
}

std::size_t LangPluginManager::_load_default_directory() {
    std::error_code ec;
    auto exe = boost::dll::program_location(ec);
    if (ec) {
        std::cerr << "[plugin] cannot resolve program location: "
                  << ec.message() << '\n';
        return 0;
    }
    // Go through a string rather than assigning the path directly: whether
    // Boost.DLL is configured with std::filesystem or boost::filesystem (the
    // BOOST_DLL_USE_STD_FS switch), .string() is available on both, so this
    // constructs a std::filesystem::path without an implicit cross-library
    // path conversion.
    std::filesystem::path exe_path(exe.string());
    return _load_directory(exe_path.parent_path() / "plugins");
}

bool LangPluginManager::is_supported(const std::filesystem::path& file_path) const noexcept {
    // Safe under noexcept: _load_directory() flushed the routing order, so
    // any_match() never triggers a (potentially throwing) lazy sort.
    return _router.any_match(file_path);
}

LangPluginManager::AnalyzePtr
LangPluginManager::make_lang_analyze(const std::filesystem::path& file_path) const {
    const Router::Entry* entry = _router.match(file_path);
    if (entry == nullptr) {
        return nullptr;
    }
    // create_instance binds entry's library into the analyzer's deleter, so the
    // .so stays mapped for the analyzer's whole lifetime.
    return _loader.create_instance(entry->plugin, entry->payload.library);
}

}
