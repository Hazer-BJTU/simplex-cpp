/**
 * @file test_lang_dispatcher.cpp
 * @brief Integration tests for LangDispatcher over the real built language plugins.
 *
 * Loads the actual python + fallback .so from LANG_PLUGIN_DIR (the same dir the
 * runtime host scans) and exercises the full path the CacheSystem relies on:
 * load + ABI-gate + warm, route by file-name regex, and mint a working analyzer
 * via the cached product_factory. (Priority tie-breaking among overlapping
 * plugins is covered by the extension_framework's own dispatcher unit tests;
 * the real python/fallback plugins deliberately do not overlap, so it is not
 * asserted here.)
 */

#define BOOST_TEST_MODULE LangDispatcherTests
#include <boost/test/unit_test.hpp>

#include "indextools/cache_system.hpp"   // LangDispatcher
#include "indextools/lang.hpp"           // LangAnalyze

#include <filesystem>
#include <string>

#ifndef LANG_PLUGIN_DIR
#error "LANG_PLUGIN_DIR must be defined by the build system"
#endif

using namespace indextools;

namespace {
/// A LangDispatcher loaded from the real built plugins. Returned by value: the
/// dispatcher is copyable (loaded contexts are shared via refcount).
LangDispatcher make_loaded_dispatcher() {
    LangDispatcher d;
    d.load_plugins(std::filesystem::path(LANG_PLUGIN_DIR));
    return d;
}
} // namespace

BOOST_AUTO_TEST_SUITE(LangDispatcherSuite)

BOOST_AUTO_TEST_CASE(loads_at_least_one_plugin) {
    auto d = make_loaded_dispatcher();
    BOOST_CHECK_GE(d.usable_count(), 1u);
}

BOOST_AUTO_TEST_CASE(routes_python_extension_and_creates_working_analyzer) {
    auto d = make_loaded_dispatcher();
    // Routing keys on the file name; the file need not exist on disk.
    auto analyzer = d.make_analyzer("module.py");
    BOOST_REQUIRE(analyzer != nullptr);

    // The minted analyzer is real: load source, analyze, and observe entities.
    analyzer->load("def foo():\n    return 1\n")->analyze();
    BOOST_CHECK(!analyzer->result().empty());
}

BOOST_AUTO_TEST_CASE(routes_fallback_extension) {
    auto d = make_loaded_dispatcher();
    // .c is claimed only by the fallback plugin (broad catch-all).
    BOOST_CHECK(d.make_analyzer("main.c") != nullptr);
}

BOOST_AUTO_TEST_CASE(unclaimed_extension_returns_null) {
    auto d = make_loaded_dispatcher();
    // .dat is not claimed by any plugin -> null, not a throw.
    BOOST_CHECK(d.make_analyzer("notes.dat") == nullptr);
}

BOOST_AUTO_TEST_CASE(missing_directory_loads_nothing) {
    // A nonexistent directory is tolerated (returns 0) rather than throwing —
    // CacheSystem relies on this constructing eagerly in any environment.
    LangDispatcher d;
    BOOST_CHECK_NO_THROW(d.load_plugins("/no/such/plugin/dir"));
    BOOST_CHECK_EQUAL(d.usable_count(), 0u);
    BOOST_CHECK(d.make_analyzer("module.py") == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
