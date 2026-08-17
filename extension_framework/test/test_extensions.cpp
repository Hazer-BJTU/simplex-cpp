/**
 * @file test_extensions.cpp
 * @brief Pure-logic / in-memory unit tests for extension_framework/extensions.hpp.
 *
 * Covers the parts of the header that do NOT require a real dynamically-loaded
 * module: the platform file predicate, the tag-generator policy, the
 * ExtensionContext base interface (bind / priority / extras), the error paths of
 * get_library_ref and create_object_from_library, the filtering + stable
 * priority sort in verify_after_loaded, load_modules_directory's per-file error
 * collection, and the one-shot load_and_verify_directory convenience (filtering +
 * error collection). Also covers the ExtensionDispatcher class (in-memory add /
 * name index, default + injected matcher/selector, stable tie-breaking, one-off
 * matcher, downcast, clear, and the virtual import_directory() override) and the
 * create_product() error paths. All driven by a runtime-created bogus .so where a
 * DSO is needed at all; the real dynamic path (including load_directory over a
 * real .so and a real create_product<Product>) is exercised by
 * test_extensions_dynamic.
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

// ---------------------------------------------------------------------------
// load_and_verify_directory: one-shot scan + load + verify + sort
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(load_and_verify_directory_empty_dir_yields_nothing) {
    scoped_temp_dir dir;
    std::vector<std::optional<std::string>> errors;
    auto verified = extension::load_and_verify_directory(
        dir.p, extension::is_likely_dynamic_library,
        extension::same_tag_always{"any"}, errors);

    BOOST_CHECK(verified.empty());
    BOOST_CHECK(errors.empty());
}

BOOST_AUTO_TEST_CASE(load_and_verify_directory_drops_bogus_so_and_records_error) {
    scoped_temp_dir dir;

    // Filtered out (wrong extension): never attempted.
    { std::ofstream(dir.p / "notes.txt") << "ignore me"; }
    // Passes the filter but is not a real ELF: load fails.
    { std::ofstream(dir.p / "broken.so", std::ios::binary) << "not a real ELF"; }

    std::vector<std::optional<std::string>> errors;
    auto verified = extension::load_and_verify_directory(
        dir.p, extension::is_likely_dynamic_library,
        extension::same_tag_always{"create_toy_extension"}, errors);

    // The one candidate failed, so nothing usable comes back...
    BOOST_CHECK(verified.empty());
    // ...but the failure is still surfaced in the errors vector.
    BOOST_REQUIRE_EQUAL(errors.size(), 1u);
    BOOST_CHECK(errors[0].has_value());
}

BOOST_AUTO_TEST_CASE(load_and_verify_directory_convenience_overload_compiles_and_runs) {
    scoped_temp_dir dir;
    // No errors& overload: empty dir -> empty verified list, errors discarded.
    auto verified = extension::load_and_verify_directory(
        dir.p, extension::is_likely_dynamic_library,
        extension::same_tag_always{"any"});
    BOOST_CHECK(verified.empty());
}

BOOST_AUTO_TEST_CASE(load_and_verify_directory_throws_for_missing_directory) {
    std::vector<std::optional<std::string>> errors;
    BOOST_CHECK_THROW(
        extension::load_and_verify_directory(
            "/no/such/directory/here", extension::is_likely_dynamic_library,
            extension::same_tag_always{"any"}, errors),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// ExtensionDispatcher: directory-driven registry + name index + key router
// ---------------------------------------------------------------------------
// Built in-memory via add() here (FakeContext), so no DSO is needed. The real
// directory-import path (load_directory) is exercised in test_extensions_dynamic
// with libtoyextension.so; the virtual import_directory() override is covered
// both there and by the local subclass below.
using DispatcherPtr = extension::ExtensionDispatcher::ContextPtr;

/// "any context matches any key" matcher — to exercise selection in isolation.
auto match_any = [](const DispatcherPtr&, std::string_view) -> bool { return true; };

BOOST_AUTO_TEST_CASE(dispatcher_starts_empty) {
    extension::ExtensionDispatcher d;
    BOOST_CHECK(d.empty());
    BOOST_CHECK_EQUAL(d.size(), 0u);
    BOOST_CHECK(d.dispatch(std::string_view("anything")) == nullptr);
    BOOST_CHECK(d.find("anything") == nullptr);
}

BOOST_AUTO_TEST_CASE(dispatcher_add_indexes_by_name) {
    extension::ExtensionDispatcher d;
    BOOST_CHECK(d.add(make_bound("python", 10)));
    BOOST_CHECK(d.add(make_bound("ruby", 5)));
    BOOST_CHECK_EQUAL(d.size(), 2u);

    BOOST_CHECK(d.contains("python"));
    BOOST_CHECK(d.find("python")->name() == "python");
    BOOST_CHECK(!d.contains("cobol"));
    BOOST_CHECK(d.find("cobol") == nullptr);
}

BOOST_AUTO_TEST_CASE(dispatcher_add_rejects_null) {
    extension::ExtensionDispatcher d;
    BOOST_CHECK(!d.add(nullptr));
    BOOST_CHECK(d.empty());
}

BOOST_AUTO_TEST_CASE(dispatcher_first_name_wins_in_index) {
    // A second context under an existing name does not overwrite find(); both stay
    // in the list (so dispatch can still reach the later one).
    extension::ExtensionDispatcher d;
    d.add(make_bound("dup", 1));
    d.add(make_bound("dup", 9));
    BOOST_CHECK_EQUAL(d.size(), 2u);
    BOOST_CHECK_EQUAL(d.find("dup")->priority(), 1); // earliest registered wins
}

BOOST_AUTO_TEST_CASE(dispatcher_dispatch_default_matches_by_name) {
    extension::ExtensionDispatcher d;
    d.add(make_bound("python", 10));
    d.add(make_bound("ruby", 5));
    auto chosen = d.dispatch(std::string_view("ruby"));
    BOOST_REQUIRE(chosen != nullptr);
    BOOST_CHECK(chosen->name() == "ruby");
}

BOOST_AUTO_TEST_CASE(dispatcher_dispatch_returns_null_when_no_match) {
    extension::ExtensionDispatcher d;
    d.add(make_bound("python", 10));
    BOOST_CHECK(d.dispatch(std::string_view("cobol")) == nullptr);
}

BOOST_AUTO_TEST_CASE(dispatcher_dispatch_picks_highest_priority_among_matches) {
    extension::ExtensionDispatcher d;
    d.set_matcher(match_any);
    d.add(make_bound("low", 1));
    d.add(make_bound("high", 100));
    d.add(make_bound("mid", 50));
    auto chosen = d.dispatch(std::string_view("any"));
    BOOST_REQUIRE(chosen != nullptr);
    BOOST_CHECK(chosen->name() == "high");
}

BOOST_AUTO_TEST_CASE(dispatcher_dispatch_ties_break_first_seen) {
    // Equal priorities: the first-seen match is kept (stable selection).
    extension::ExtensionDispatcher d;
    d.set_matcher(match_any);
    d.add(make_bound("first", 5));
    d.add(make_bound("second", 5));
    BOOST_CHECK(d.dispatch(std::string_view("k"))->name() == "first");
}

BOOST_AUTO_TEST_CASE(dispatcher_set_selector_overrides_default) {
    // Inject a selector that prefers LOWER priority (reverse of the default).
    extension::ExtensionDispatcher d;
    d.set_matcher(match_any);
    d.set_selector([](const DispatcherPtr& a, const DispatcherPtr& b) {
        return a->priority() < b->priority();
    });
    d.add(make_bound("hi", 100));
    d.add(make_bound("lo", 1));
    BOOST_CHECK(d.dispatch(std::string_view("k"))->name() == "lo");
}

BOOST_AUTO_TEST_CASE(dispatcher_one_off_matcher_ignores_bound_one) {
    // dispatch(key, matcher) uses the ad-hoc matcher, not the bound one.
    extension::ExtensionDispatcher d;
    d.add(make_bound("python", 10));
    auto by_priority = [](const DispatcherPtr& c, std::string_view) {
        return c->priority() == 10;
    };
    // The bound matcher (match by name) would miss "nomatch"; the ad-hoc one hits.
    BOOST_CHECK(d.dispatch(std::string_view("nomatch"), by_priority) != nullptr);
}

BOOST_AUTO_TEST_CASE(dispatcher_downcast_reaches_concrete_type) {
    // The caller downcasts the returned base pointer to the concrete type to
    // reach category-specific state — the framework stays generic.
    extension::ExtensionDispatcher d;
    d.add(make_bound("concrete", 10));
    auto chosen = d.dispatch(std::string_view("concrete"));
    BOOST_REQUIRE(chosen != nullptr);
    auto concrete = std::dynamic_pointer_cast<FakeContext>(chosen);
    BOOST_REQUIRE(concrete != nullptr);
    BOOST_CHECK_EQUAL(concrete->priority(), 10);
}

BOOST_AUTO_TEST_CASE(dispatcher_clear_drops_everything) {
    extension::ExtensionDispatcher d;
    d.add(make_bound("a", 1));
    d.add(make_bound("b", 2));
    d.clear();
    BOOST_CHECK(d.empty());
    BOOST_CHECK(!d.contains("a"));
    BOOST_CHECK(d.dispatch(std::string_view("a")) == nullptr);
}

BOOST_AUTO_TEST_CASE(dispatcher_import_directory_override_is_used) {
    // A subclass overrides the virtual import_directory() to source contexts
    // differently; load_directory() must route through it and still index the
    // results. This is the "supports inheritance + override" contract.
    struct CustomDispatcher : extension::ExtensionDispatcher {
        std::vector<ContextPtr> import_directory(
            const std::filesystem::path&,
            Filter, TagGenerator,
            std::vector<std::optional<std::string>>&, bool) override {
            // Hand-built contexts instead of a real directory scan; the path and
            // filter/tag args are deliberately ignored.
            return {make_bound("custom-a", 3), make_bound("custom-b", 7)};
        }
    };
    CustomDispatcher d;
    std::vector<std::optional<std::string>> errors;
    auto added = d.load_directory("/any/path/ignored",
                                  extension::is_likely_dynamic_library,
                                  extension::same_tag_always{"x"}, errors);
    BOOST_CHECK_EQUAL(added, 2u);
    BOOST_CHECK_EQUAL(d.size(), 2u);
    BOOST_CHECK(d.contains("custom-a"));
    BOOST_CHECK_EQUAL(d.find("custom-b")->priority(), 7);
}

// ---------------------------------------------------------------------------
// create_product: mint a Product from a context via create_object_from_library
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(create_product_rejects_null_context) {
    BOOST_CHECK_THROW(
        extension::create_product<FakeContext>(nullptr, "any_alias"),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(create_product_rejects_unbound_context) {
    // A context that was never bound has a null library handle: forwarded to
    // create_object_from_library, which rejects it.
    auto unbound = std::make_shared<FakeContext>("u", 0);
    BOOST_CHECK_THROW(
        extension::create_product<FakeContext>(unbound, "any_alias"),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(create_product_surfaces_missing_alias) {
    // make_bound() attaches an *unloaded* shared_library, whose has() is always
    // false, so any alias lookup fails — without needing a real .so on disk.
    auto bound = make_bound("b", 0);
    BOOST_CHECK_THROW(
        extension::create_product<FakeContext>(bound, "no_such_alias"),
        std::runtime_error);
}

// ---------------------------------------------------------------------------
// product_factory: resolve-once cached factory handle
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(product_factory_rejects_null_library) {
    // Whole expression parenthesized so the commas in the template args and the
    // ctor call are not seen as BOOST_CHECK_THROW macro-arg separators.
    BOOST_CHECK_THROW(
        (extension::product_factory<FakeContext>(nullptr, "any_alias")),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(product_factory_rejects_missing_alias) {
    // An unloaded shared_library has() == false for every alias.
    auto stub = std::make_shared<boost::dll::shared_library>();
    BOOST_CHECK_THROW(
        (extension::product_factory<FakeContext>(stub, "no_such_alias")),
        std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
