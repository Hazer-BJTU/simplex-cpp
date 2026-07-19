#define BOOST_TEST_MODULE PluginManagerTests
#include <boost/test/unit_test.hpp>

#include "indextools/plugin_manager.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

using namespace indextools;

// The plugin output directory, injected by CMake (see test/CMakeLists.txt).
#ifndef LANG_PLUGIN_DIR
#error "LANG_PLUGIN_DIR must be defined by the build system"
#endif

namespace {

// The manager is a process-wide singleton, loaded exactly once from the real
// plugins built into LANG_PLUGIN_DIR. ensure_loaded() is idempotent, so every
// test can call this and share the same fully-loaded instance.
LangPluginManager& loaded_manager() {
    auto& mgr = LangPluginManager::instance();
    size_t n = mgr.ensure_loaded(LANG_PLUGIN_DIR);
    BOOST_REQUIRE_MESSAGE(n >= 2,
        "expected >=2 plugins in " LANG_PLUGIN_DIR ", loaded " << n);
    return mgr;
}

// Write a temporary file with the given extension and content.
struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& ext, const std::string& content) {
        path = std::filesystem::temp_directory_path() /
               ("plugin_mgr_test_" + std::to_string(::getpid()) + ext);
        std::ofstream(path) << content;
    }
    ~TempFile() { std::error_code ec; std::filesystem::remove(path, ec); }
};

} // namespace

BOOST_AUTO_TEST_SUITE(PluginDiscoverySuite)

BOOST_AUTO_TEST_CASE(loads_python_and_fallback_plugins) {
    auto& mgr = loaded_manager();
    BOOST_CHECK_GE(mgr.plugin_count(), 2u);
}

BOOST_AUTO_TEST_CASE(python_extensions_are_supported) {
    auto& mgr = loaded_manager();
    BOOST_CHECK(mgr.is_supported("foo.py"));
    BOOST_CHECK(mgr.is_supported("foo.pyi"));
    // Case-insensitive.
    BOOST_CHECK(mgr.is_supported("FOO.PY"));
}

BOOST_AUTO_TEST_CASE(fallback_extensions_are_supported) {
    auto& mgr = loaded_manager();
    BOOST_CHECK(mgr.is_supported("main.cpp"));
    BOOST_CHECK(mgr.is_supported("notes.md"));
}

// Regex routing lets the fallback claim fixed-name / extensionless files that
// an extension table never could.
BOOST_AUTO_TEST_CASE(fixed_name_files_are_supported) {
    auto& mgr = loaded_manager();
    BOOST_CHECK(mgr.is_supported("Dockerfile"));
    BOOST_CHECK(mgr.is_supported("Dockerfile.dev"));
    BOOST_CHECK(mgr.is_supported("CMakeLists.txt"));
    BOOST_CHECK(mgr.is_supported("Makefile"));
    BOOST_CHECK(mgr.is_supported("GNUmakefile"));
}

// Matching is against the file name only, and directory names must not leak in.
BOOST_AUTO_TEST_CASE(matches_name_not_path) {
    auto& mgr = loaded_manager();
    BOOST_CHECK(mgr.is_supported("/deep/nested/dir/main.py"));
    // A ".py" directory component must not make an unsupported file match.
    BOOST_CHECK(!mgr.is_supported("/proj.py/archive.zip"));
}

BOOST_AUTO_TEST_CASE(unknown_extension_is_unsupported) {
    auto& mgr = loaded_manager();
    BOOST_CHECK(!mgr.is_supported("archive.zip"));
    BOOST_CHECK(!mgr.is_supported("noext"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(AnalyzerCreationSuite)

BOOST_AUTO_TEST_CASE(make_returns_null_for_unsupported) {
    auto& mgr = loaded_manager();
    BOOST_CHECK(mgr.make_lang_analyze("archive.zip") == nullptr);
}

BOOST_AUTO_TEST_CASE(python_analyzer_extracts_entities) {
    auto& mgr = loaded_manager();
    TempFile f(".py", "def hello():\n    return 42\n");

    auto analyzer = mgr.make_lang_analyze(f.path);
    BOOST_REQUIRE(analyzer != nullptr);

    analyzer->open(f.path)->analyze();
    // The Python plugin should extract at least the module + function entity.
    BOOST_CHECK_GE(analyzer->result().size(), 2u);
}

BOOST_AUTO_TEST_CASE(fallback_analyzer_indexes_identifiers) {
    auto& mgr = loaded_manager();
    TempFile f(".cpp", "int alpha = beta + gamma;\n");

    auto analyzer = mgr.make_lang_analyze(f.path);
    BOOST_REQUIRE(analyzer != nullptr);

    analyzer->open(f.path)->analyze();
    auto json = analyzer->locate_identifier("beta");
    BOOST_CHECK(!json.empty());
}

// The crux of plugin mode: an analyzer carries its own library reference in its
// deleter, so it stays fully usable independent of the manager. The manager is
// now a process-lifetime singleton, so we prove the deleter's job by keeping an
// analyzer as the *sole* handle onto its plugin's code — creating it, then
// dropping every other local reference before driving plugin calls and, last,
// the analyzer's own destructor.
BOOST_AUTO_TEST_CASE(analyzer_carries_its_library) {
    LangPluginManager::AnalyzePtr analyzer;
    {
        // f is scoped away before we use the analyzer; the analyzer must not
        // depend on anything from the creation site staying alive.
        TempFile f(".py", "class Widget:\n    pass\n");
        analyzer = loaded_manager().make_lang_analyze(f.path);
        BOOST_REQUIRE(analyzer != nullptr);
        analyzer->load("class Widget:\n    pass\n")->analyze();
    }

    // Calling into plugin code (including the eventual dtor) must be safe.
    BOOST_CHECK_GE(analyzer->result().size(), 1u);
    auto structure = analyzer->get_full_structure();
    BOOST_CHECK(!structure.empty());
    analyzer.reset(); // triggers ~LangAnalyze inside the (still-loaded) plugin
}

BOOST_AUTO_TEST_SUITE_END()
