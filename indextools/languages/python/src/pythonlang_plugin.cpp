/**
 * @file pythonlang_plugin.cpp
 * @brief Plugin export wrapper for the Python language analyzer.
 *
 * Turns PythonLanguage into a loadable plugin: it declares the supported
 * extensions (previously listed in languages/language_config.json) and exports
 * the factory alias the host's LangPluginManager imports. The analysis logic
 * itself lives untouched in pythonlang.cpp.
 */

#include "pythonlang.hpp"
#include "lang_plugin.hpp"

#include <boost/dll/alias.hpp>

namespace indextools {

namespace {

class PythonPlugin final : public LangPlugin {
public:
    std::uint32_t abi_version() const noexcept override {
        return LANG_PLUGIN_ABI_VERSION;
    }

    std::string_view name() const noexcept override {
        return "Python";
    }

    // Matches .py / .pyw / .pyi (case-insensitive, anchored at end of name).
    std::string_view file_pattern() const noexcept override {
        return R"(\.py[wi]?$)";
    }

    std::unique_ptr<LangAnalyze> create() const override {
        return std::make_unique<PythonLanguage>();
    }
};

} // namespace

// Exported factory: minted inside the plugin module so the returned object's
// vtable/code belong to this library.
std::shared_ptr<LangPlugin> create_lang_plugin() {
    return std::make_shared<PythonPlugin>();
}

} // namespace indextools

BOOST_DLL_ALIAS(indextools::create_lang_plugin, create_lang_plugin)
