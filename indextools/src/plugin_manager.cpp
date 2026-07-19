#include "indextools/plugin_manager.hpp"

#include <algorithm>
#include <iostream>

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/dll/shared_library_load_mode.hpp>

namespace indextools {

LangPluginManager& LangPluginManager::instance() {
    // Function-local static: thread-safe one-time construction (C++11 [stmt.dcl]).
    static LangPluginManager manager;
    return manager;
}

size_t LangPluginManager::ensure_loaded() {
    std::call_once(_load_once, [this] { load_default_directory(); });
    return _plugins.size();
}

size_t LangPluginManager::ensure_loaded(const std::filesystem::path& directory) {
    std::call_once(_load_once, [this, &directory] { load_directory(directory); });
    return _plugins.size();
}

bool LangPluginManager::_is_dynamic_library(const std::filesystem::path& path) noexcept {
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

bool LangPluginManager::load(const std::filesystem::path& library_path) {
    namespace dll = boost::dll;

    std::shared_ptr<dll::shared_library> library;
    std::shared_ptr<LangPlugin> plugin;
    try {
        library = std::make_shared<dll::shared_library>(
            library_path, dll::load_mode::append_decorations);

        if (!library->has(LANG_PLUGIN_FACTORY_NAME)) {
            std::cerr << "[plugin] skip " << library_path
                      << ": missing symbol '" << LANG_PLUGIN_FACTORY_NAME << "'\n";
            return false;
        }

        // import_alias returns a callable that itself holds a library reference;
        // we invoke it once to mint the descriptor, then drop it. The descriptor
        // (living inside the library) is kept alive by the LoadedPlugin holding
        // `library` directly.
        auto factory = dll::import_alias<LangPluginFactory>(
            *library, LANG_PLUGIN_FACTORY_NAME);
        plugin = factory();
    } catch (const std::exception& e) {
        std::cerr << "[plugin] failed to load " << library_path
                  << ": " << e.what() << '\n';
        return false;
    }

    if (!plugin) {
        std::cerr << "[plugin] " << library_path
                  << ": factory returned nullptr\n";
        return false;
    }
    if (plugin->abi_version() != LANG_PLUGIN_ABI_VERSION) {
        std::cerr << "[plugin] " << library_path << " (" << plugin->name()
                  << "): ABI version mismatch — plugin " << plugin->abi_version()
                  << ", host " << LANG_PLUGIN_ABI_VERSION << "; skipped\n";
        return false;
    }

    // Compile the file-name matcher host-side. A bad pattern rejects the whole
    // plugin: silently loading an analyzer that can never be routed to would be
    // more confusing than a clear diagnostic.
    std::regex pattern;
    try {
        pattern = std::regex(std::string(plugin->file_pattern()),
                             std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error& e) {
        std::cerr << "[plugin] " << library_path << " (" << plugin->name()
                  << "): invalid file_pattern() '" << plugin->file_pattern()
                  << "': " << e.what() << "; skipped\n";
        return false;
    }

    _plugins.push_back(LoadedPlugin{std::move(library), plugin, std::move(pattern)});
    return true;
}

void LangPluginManager::_finalize_order() {
    // Stable sort by priority descending: highest priority routes first, and
    // equal-priority plugins keep their load order. A catch-all reporting a low
    // priority thus ends up last and is only chosen when nothing else matches.
    std::stable_sort(_plugins.begin(), _plugins.end(),
                     [](const LoadedPlugin& a, const LoadedPlugin& b) {
                         return a.plugin->priority() > b.plugin->priority();
                     });
}

size_t LangPluginManager::load_directory(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        std::cerr << "[plugin] not a directory: " << directory << '\n';
        return 0;
    }

    size_t loaded = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || !_is_dynamic_library(entry.path())) {
            continue;
        }
        if (load(entry.path())) {
            ++loaded;
        }
    }
    // Directory iteration order is unspecified, so priority — not filesystem
    // order — must decide routing. Freeze that order once the batch is in.
    _finalize_order();
    return loaded;
}

size_t LangPluginManager::load_default_directory() {
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
    return load_directory(exe_path.parent_path() / "plugins");
}

size_t LangPluginManager::_match_plugin(const std::filesystem::path& file_path) const noexcept {
    // Match against the file *name* so extensionless / fixed-name files
    // (Dockerfile, CMakeLists.txt) can be claimed, not just ".ext" suffixes.
    const std::string name = file_path.filename().string();
    for (size_t i = 0; i < _plugins.size(); ++i) {
        // regex_search (not regex_match): patterns are conventionally anchored
        // suffixes like `\.py$`, not whole-name matches.
        if (std::regex_search(name, _plugins[i].pattern)) {
            return i;
        }
    }
    return static_cast<size_t>(-1);
}

bool LangPluginManager::is_supported(const std::filesystem::path& file_path) const noexcept {
    return _match_plugin(file_path) != static_cast<size_t>(-1);
}

LangPluginManager::AnalyzePtr
LangPluginManager::make_lang_analyze(const std::filesystem::path& file_path) const {
    const size_t idx = _match_plugin(file_path);
    if (idx == static_cast<size_t>(-1)) {
        return nullptr;
    }

    const LoadedPlugin& lp = _plugins[idx];
    std::unique_ptr<LangAnalyze> raw = lp.plugin->create();
    if (!raw) {
        return nullptr;
    }

    // Bind the owning library into the deleter. The analyzer's dtor (code in the
    // plugin) runs first; the captured library reference is released afterwards,
    // so the library stays mapped for the analyzer's entire lifetime.
    std::shared_ptr<boost::dll::shared_library> lib = lp.library;
    return AnalyzePtr(raw.release(),
        [lib = std::move(lib)](LangAnalyze* p) noexcept {
            delete p;
            // `lib` drops here, after the object is fully destroyed.
        });
}

}
