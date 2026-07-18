/**
 * @file fallbacklang_plugin.cpp
 * @brief Plugin export wrapper for the generic fallback analyzer.
 *
 * Matches a broad set of source/config/text files by name, plus a few
 * fixed-name files that have no extension (Dockerfile, Makefile, CMakeLists.txt).
 * It reports LANG_PLUGIN_FALLBACK_PRIORITY, so any dedicated language plugin —
 * present or added later — outranks it automatically. There is therefore no
 * need to prune this pattern when a real plugin appears: the host prefers the
 * higher-priority match and only falls through to here when nothing else claims
 * the file.
 */

#include "fallbacklang.hpp"
#include "lang_plugin.hpp"

#include <boost/dll/alias.hpp>

namespace indextools {

namespace {

class FallbackPlugin final : public LangPlugin {
public:
    std::uint32_t abi_version() const noexcept override {
        return LANG_PLUGIN_ABI_VERSION;
    }

    std::string_view name() const noexcept override {
        return "Fallback";
    }

    // One regex for every file the fallback claims, matched case-insensitively
    // against the file name. Two branches:
    //   1. a `.<ext>$` alternation of the broad extension set, and
    //   2. fixed-name / no-extension files (Dockerfile, Makefile, CMakeLists.txt),
    //      including common suffixed variants like "Dockerfile.dev" or
    //      "GNUmakefile".
    // Kept as one string so the whole matcher lives in one place; the host
    // compiles it once at load.
    std::string_view file_pattern() const noexcept override {
        return
            // fixed-name files (anchored at start of name)
            R"((^(Dockerfile|Containerfile)(\..+)?$)|(^([A-Za-z]*[Mm]akefile)$)|(^CMakeLists\.txt$)|)"
            // extension-based files (anchored at end of name)
            R"((\.(c|h|cpp|cc|cxx|hpp|hxx|hh|cs|java|kt|kts|scala|js|mjs|cjs|jsx|ts|tsx|)"
            R"(go|rs|rb|php|swift|dart|lua|pl|pm|r|sh|bash|zsh|fish|ps1|sql|proto|)"
            R"(graphql|gql|html|htm|css|scss|sass|less|vue|svelte|ml|hs|erl|ex|exs|)"
            R"(jl|clj|cljs|lisp|el|zig|nim|mk|cmake|gradle|xml|svg|xsd|xsl|xslt|)"
            R"(yaml|yml|toml|json|ini|cfg|conf|config|properties|md|markdown|mdx|)"
            R"(rst|txt|text|log|csv|tsv|tex|bib|vim)$))";
    }

    // Lowest priority: any dedicated language plugin outranks the catch-all.
    int priority() const noexcept override {
        return LANG_PLUGIN_FALLBACK_PRIORITY;
    }

    std::unique_ptr<LangAnalyze> create() const override {
        return std::make_unique<FallbackLanguage>();
    }
};

} // namespace

std::shared_ptr<LangPlugin> create_lang_plugin() {
    return std::make_shared<FallbackPlugin>();
}

} // namespace indextools

BOOST_DLL_ALIAS(indextools::create_lang_plugin, create_lang_plugin)
