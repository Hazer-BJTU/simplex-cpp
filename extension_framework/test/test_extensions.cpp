/**
 * @file test_extensions.cpp
 * @brief Pure-logic / in-memory unit tests for extension_framework/extensions.hpp.
 *
 * Covers the parts of the header that do NOT require a real dynamically-loaded
 * module: the platform file predicate, the tag-generator policy, the
 * ExtensionContext base interface (bind / priority / extras), the error paths of
 * get_library_ref and create_object_from_library, the filtering + stable
 * priority sort in verify_after_loaded, and load_modules_directory's per-file
 * error collection (driven by a runtime-created bogus .so, so no build-time DSO
 * is needed here). The real dynamic path is exercised by test_extensions_dynamic.
 */

#define BOOST_TEST_MODULE ExtensionFrameworkStaticTests
#include <boost/test/unit_test.hpp>

#include "extension_framework/extensions.hpp"

#include <boost/dll/shared_library.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

/// RAII temporary directory, removed on destruction. A fixed name is safe because
/// only this binary uses it and Boost.Test cases within a binary run sequentially.
struct scoped_temp_dir {
    std::filesystem::path p;
    scoped_temp_dir() {
        p = std::filesystem::temp_directory_path() / "simplex_ext_test_dir";
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
    }
    ~scoped_temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
    }
};

/// Concrete ExtensionContext for in-memory tests, with configurable name/priority.
struct FakeContext : extension::ExtensionContext {
    std::string n;
    long prio = 0;
    FakeContext(std::string name, long priority) : n(std::move(name)), prio(priority) {}
    std::uint32_t abi_version() const noexcept override { return 1; }
    std::string_view name() const noexcept override { return n; }
    long priority() const noexcept override { return prio; }
};

/// Build a FakeContext with a (non-null) bound library handle so it survives
/// verify_after_loaded's library-ref check. The handle is an unloaded
/// shared_library; verify only inspects the pointer, not is_loaded().
std::shared_ptr<extension::ExtensionContext> make_bound(std::string name, long prio) {
    auto ctx = std::make_shared<FakeContext>(std::move(name), prio);
    ctx->bind(std::make_shared<boost::dll::shared_library>());
    return ctx;
}

} // namespace

BOOST_AUTO_TEST_SUITE(ExtensionsStaticSuite)

// ---------------------------------------------------------------------------
// is_likely_dynamic_library
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(dynamic_library_predicate_recognizes_extensions) {
    namespace fs = std::filesystem;
    using extension::is_likely_dynamic_library;

#if defined(__linux__) || defined(__unix__) || defined(__unix)
    BOOST_CHECK(is_likely_dynamic_library(fs::path("libfoo.so")));
    BOOST_CHECK(is_likely_dynamic_library(fs::path("foo.so")));
    BOOST_CHECK(is_likely_dynamic_library(fs::path("libfoo.so.1")));   // versioned ELF
    BOOST_CHECK(is_likely_dynamic_library(fs::path("/abs/path/libfoo.so.5.2")));
    BOOST_CHECK(is_likely_dynamic_library(fs::path("foo.SO")));        // case-insensitive
    BOOST_CHECK(!is_likely_dynamic_library(fs::path("foo.txt")));
    BOOST_CHECK(!is_likely_dynamic_library(fs::path("foo")));          // no extension
    BOOST_CHECK(!is_likely_dynamic_library(fs::path("foo.dylib")));    // not a Linux lib
#elif defined(__APPLE__)
    BOOST_CHECK(is_likely_dynamic_library(fs::path("libfoo.dylib")));
    BOOST_CHECK(is_likely_dynamic_library(fs::path("libfoo.so")));
    BOOST_CHECK(is_likely_dynamic_library(fs::path("foo.bundle")));
    BOOST_CHECK(!is_likely_dynamic_library(fs::path("foo.txt")));
#elif defined(_WIN32)
    BOOST_CHECK(is_likely_dynamic_library(fs::path("foo.dll")));
    BOOST_CHECK(is_likely_dynamic_library(fs::path("foo.ocx")));
    BOOST_CHECK(!is_likely_dynamic_library(fs::path("foo.txt")));
#endif
    // An empty path must not throw (the noexcept guard swallows it).
    BOOST_CHECK_NO_THROW(is_likely_dynamic_library(fs::path("")));
}

// ---------------------------------------------------------------------------
// same_tag_always
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(same_tag_always_returns_constant_tag) {
    extension::same_tag_always gen{"create_toy_extension"};
    BOOST_CHECK(gen("anything.so") == "create_toy_extension");
    BOOST_CHECK(gen("/some/other/path.dylib") == "create_toy_extension");
}

// ---------------------------------------------------------------------------
// ExtensionContext base interface
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(context_bind_rejects_null_library) {
    FakeContext ctx{"x", 0};
    BOOST_CHECK_THROW(ctx.bind(nullptr), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(context_bind_then_get_library_ref_roundtrips) {
    FakeContext ctx{"x", 0};
    BOOST_CHECK(ctx.get_library_ref() == nullptr); // unbound before bind()
    auto lib = std::make_shared<boost::dll::shared_library>();
    ctx.bind(lib);
    BOOST_CHECK(ctx.get_library_ref() == lib);
}

BOOST_AUTO_TEST_CASE(context_defaults_for_priority_and_extras) {
    FakeContext ctx{"x", extension::ExtensionContext::DEFAULT_CONTEXT_PRIORITY};
    BOOST_CHECK_EQUAL(ctx.priority(), extension::ExtensionContext::DEFAULT_CONTEXT_PRIORITY);
    BOOST_CHECK_EQUAL(ctx.priority(), 0);
    BOOST_CHECK(ctx.extras().is_null());
    BOOST_CHECK_EQUAL(ctx.abi_version(), 1u);
    BOOST_CHECK(ctx.name() == "x");
}

// ---------------------------------------------------------------------------
// get_library_ref error path
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(get_library_ref_throws_on_missing_path) {
    BOOST_CHECK_THROW(
        extension::get_library_ref("/nonexistent/path/no_such_library.so"),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// create_object_from_library error paths
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(create_object_rejects_null_library) {
    BOOST_CHECK_THROW(
        extension::create_object_from_library<FakeContext>(nullptr, "alias"),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(create_object_throws_on_missing_alias) {
    // A default-constructed (unloaded) library reports has() == false, so any
    // alias lookup fails without needing a real .so on disk.
    auto stub = std::make_shared<boost::dll::shared_library>();
    BOOST_CHECK_THROW(
        extension::create_object_from_library<FakeContext>(stub, "no_such_alias"),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// verify_after_loaded: filtering + ordering
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(verify_drops_failed_loads_and_keeps_good_ones) {
    auto good = make_bound("good", 1);
    std::vector<std::shared_ptr<extension::ExtensionContext>> loaded{
        nullptr, good, nullptr};
    std::vector<std::optional<std::string>> errors{
        "load failed A", std::nullopt, "load failed B"};

    auto verified = extension::verify_after_loaded(loaded, errors);
    BOOST_REQUIRE_EQUAL(verified.size(), 1u);
    BOOST_CHECK(verified[0]->name() == "good");
}

BOOST_AUTO_TEST_CASE(verify_drops_context_without_library_ref) {
    // A context that was never bound has a null library handle and must be dropped.
    auto unbound = std::make_shared<FakeContext>("unbound", 1);
    auto bound = make_bound("bound", 1);

    auto verified = extension::verify_after_loaded({unbound, bound}, {std::nullopt, std::nullopt});
    BOOST_REQUIRE_EQUAL(verified.size(), 1u);
    BOOST_CHECK(verified[0]->name() == "bound");
}

BOOST_AUTO_TEST_CASE(verify_sorts_by_priority_descending) {
    auto low = make_bound("low", 1);
    auto high = make_bound("high", 100);
    auto mid = make_bound("mid", 50);

    auto verified = extension::verify_after_loaded(
        {low, high, mid}, {std::nullopt, std::nullopt, std::nullopt});
    BOOST_REQUIRE_EQUAL(verified.size(), 3u);
    BOOST_CHECK(verified[0]->name() == "high");
    BOOST_CHECK(verified[1]->name() == "mid");
    BOOST_CHECK(verified[2]->name() == "low");
}

BOOST_AUTO_TEST_CASE(verify_sort_is_stable_for_equal_priorities) {
    // Insertion order: first(5), second(5), third(5). Stable sort must preserve it.
    auto first = make_bound("first", 5);
    auto second = make_bound("second", 5);
    auto third = make_bound("third", 5);

    auto verified = extension::verify_after_loaded(
        {first, second, third}, {std::nullopt, std::nullopt, std::nullopt});
    BOOST_REQUIRE_EQUAL(verified.size(), 3u);
    BOOST_CHECK(verified[0]->name() == "first");
    BOOST_CHECK(verified[1]->name() == "second");
    BOOST_CHECK(verified[2]->name() == "third");
}

BOOST_AUTO_TEST_CASE(verify_tolerates_mismatched_errors_vector) {
    // loaded has 2 nulls but errors is empty: must not read out of bounds.
    auto verified = extension::verify_after_loaded(
        std::vector<std::shared_ptr<extension::ExtensionContext>>{nullptr, nullptr},
        std::vector<std::optional<std::string>>{});
    BOOST_CHECK(verified.empty());
}

BOOST_AUTO_TEST_CASE(verify_on_empty_input_returns_empty) {
    auto verified = extension::verify_after_loaded(
        std::vector<std::shared_ptr<extension::ExtensionContext>>{},
        std::vector<std::optional<std::string>>{});
    BOOST_CHECK(verified.empty());
}

// ---------------------------------------------------------------------------
// load_modules_directory: discovery + per-file error collection
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(load_directory_empty_dir_yields_nothing) {
    scoped_temp_dir dir;
    std::vector<std::optional<std::string>> errors;
    auto loaded = extension::load_modules_directory(
        dir.p, extension::is_likely_dynamic_library,
        extension::same_tag_always{"any"}, errors);

    BOOST_CHECK(loaded.empty());
    BOOST_CHECK(errors.empty());
}

BOOST_AUTO_TEST_CASE(load_directory_records_failure_for_bogus_so) {
    scoped_temp_dir dir;

    // A non-library file is filtered out and never attempted.
    { std::ofstream(dir.p / "readme.txt") << "ignore me"; }
    // A .so-named file with garbage content passes the filter but fails to load.
    { std::ofstream(dir.p / "broken.so", std::ios::binary) << "not a real ELF"; }

    std::vector<std::optional<std::string>> errors;
    auto loaded = extension::load_modules_directory(
        dir.p, extension::is_likely_dynamic_library,
        extension::same_tag_always{"create_toy_extension"}, errors);

    // Exactly one candidate (broken.so) was attempted; it failed.
    BOOST_REQUIRE_EQUAL(loaded.size(), 1u);
    BOOST_CHECK(loaded[0] == nullptr);
    BOOST_REQUIRE_EQUAL(errors.size(), 1u);
    BOOST_CHECK(errors[0].has_value());

    // verify_after_loaded filters it out cleanly.
    auto verified = extension::verify_after_loaded(loaded, errors);
    BOOST_CHECK(verified.empty());
}

BOOST_AUTO_TEST_CASE(load_directory_convenience_overload_compiles_and_runs) {
    scoped_temp_dir dir;
    // No errors& overload: should return an empty vector for an empty dir.
    auto loaded = extension::load_modules_directory(
        dir.p, extension::is_likely_dynamic_library,
        extension::same_tag_always{"any"});
    BOOST_CHECK(loaded.empty());
}

BOOST_AUTO_TEST_CASE(load_directory_throws_for_missing_directory) {
    std::vector<std::optional<std::string>> errors;
    BOOST_CHECK_THROW(
        extension::load_modules_directory(
            "/no/such/directory/here", extension::is_likely_dynamic_library,
            extension::same_tag_always{"any"}, errors),
        std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
