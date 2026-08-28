/**
 * @file test_extensions_dynamic.cpp
 * @brief Dynamic-loading tests for the extension framework, driven by a real DSO.
 *
 * Builds against libtoyextension.so (a concrete ExtensionContext) and exercises
 * the full pipeline end to end:
 *   - get_library_ref + create_object_from_library + bind (the happy path)
 *   - the deleter-pinned ownership mode (bind_library_ref_deleter = true)
 *   - wrong-alias rejection
 *   - load_modules_directory + verify_after_loaded over the DSO directory
 *   - load_and_verify_directory (the one-shot composed pipeline) over the DSO dir
 *   - ExtensionDispatcher.load_directory -> dispatch -> create_product end-to-end
 *     (build the name index from the DSO, dispatch by name, then mint a real
 *     ToyProduct via create_object_from_library through create_product)
 *   - product_factory: resolve the alias once, then create() many ToyProducts
 *     from the cached function pointer (no re-import), plus deleter-pinned
 *     survival past the factory's destruction
 *
 * Together with test_extensions.cpp (pure-logic coverage), this validates every
 * code path in extensions.hpp.
 */

#define BOOST_TEST_MODULE ExtensionFrameworkDynamicTests
#include <boost/test/unit_test.hpp>

#include "extension_framework/extensions.hpp"
#include "toy_extension_spec.hpp"

#include <boost/dll/shared_library.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef TOY_EXTENSION_DIR
#error "TOY_EXTENSION_DIR must be defined by the build system"
#endif

namespace {

/// Path to the built toy extension module.
std::filesystem::path toy_extension_path() {
    // The CMake target prefix is "lib"; on Linux the file is libtoyextension.so.
    return std::filesystem::path(TOY_EXTENSION_DIR) / "libtoyextension.so";
}

#ifndef TOY_BAD_MAGIC_DIR
#error "TOY_BAD_MAGIC_DIR must be defined by the build system"
#endif

/// Path to the fixture whose magic block fingerprints a different toolchain.
std::filesystem::path bad_magic_path() {
    return std::filesystem::path(TOY_BAD_MAGIC_DIR) / "libtoyextension_bad_magic.so";
}

/// Path to the fixture with no magic block at all (legacy/hand-rolled shape).
std::filesystem::path legacy_module_path() {
    return std::filesystem::path(TOY_BAD_MAGIC_DIR) / "libtoyextension_legacy.so";
}

} // namespace

BOOST_AUTO_TEST_SUITE(ExtensionDynamicSuite)

// Happy path: open the library, import the factory, bind the handle, read metadata.
BOOST_AUTO_TEST_CASE(loads_toy_extension_and_reads_metadata) {
    namespace ext = extension;
    auto path = toy_extension_path();
    BOOST_REQUIRE(std::filesystem::exists(path));

    auto library_ref = ext::get_library_ref(path);
    BOOST_REQUIRE(library_ref != nullptr);

    auto ctx = ext::create_object_from_library<ext::ExtensionContext, false>(
        library_ref, ext_test::TOY_EXTENSION_FACTORY_NAME);
    BOOST_REQUIRE(ctx != nullptr);
    BOOST_CHECK(ctx->get_library_ref() == nullptr); // not bound yet

    ctx->bind(library_ref);
    BOOST_CHECK(ctx->get_library_ref() != nullptr);

    BOOST_CHECK_EQUAL(ctx->abi_version(), ext_test::TOY_EXTENSION_ABI_VERSION);
    BOOST_CHECK(ctx->name() == ext_test::TOY_EXTENSION_NAME);
    BOOST_CHECK_EQUAL(ctx->priority(), ext_test::TOY_EXTENSION_PRIORITY);
    BOOST_CHECK(ctx->extras().is_null()); // default impl
}

// The deleter-pinned mode: the library handle is bound into the shared_ptr's
// deleter, so no separate bind() is required. Must not crash on destruction.
BOOST_AUTO_TEST_CASE(deleter_pinned_mode_keeps_library_alive_for_object_lifetime) {
    namespace ext = extension;
    auto library_ref = ext::get_library_ref(toy_extension_path());

    {
        auto ctx = ext::create_object_from_library<ext::ExtensionContext, true>(
            library_ref, ext_test::TOY_EXTENSION_FACTORY_NAME);
        BOOST_REQUIRE(ctx != nullptr);
        BOOST_CHECK(ctx->name() == ext_test::TOY_EXTENSION_NAME);
        // The deleter captured library_ref; destructing ctx here runs the
        // concrete destructor (in the .so) while the handle is still mapped.
    } // no crash past this point
}

// A wrong alias name must be rejected with a runtime_error (missing-symbol path).
BOOST_AUTO_TEST_CASE(wrong_alias_is_rejected) {
    namespace ext = extension;
    auto library_ref = ext::get_library_ref(toy_extension_path());
    // The expression is wrapped in extra parens so the commas inside the
    // template-args <...> and the function call are not seen as macro-arg
    // separators by BOOST_CHECK_THROW (a 2-argument macro).
    BOOST_CHECK_THROW(
        (ext::create_object_from_library<ext::ExtensionContext, false>(
            library_ref, "definitely_not_an_exported_alias")),
        std::runtime_error);
}

// A factory that returns null must be rejected as a load failure. Before the
// null guard this produced a null-stored-pointer shared_ptr (unique_ptr→
// shared_ptr does NOT throw), which load_modules_directory then dereferenced
// (bind through a null object) — one broken module crashed the whole host
// instead of being recorded as a per-file error.
BOOST_AUTO_TEST_CASE(null_factory_result_is_rejected) {
    namespace ext = extension;
    auto library_ref = ext::get_library_ref(toy_extension_path());

    BOOST_CHECK_THROW(
        (ext::create_object_from_library<ext::ExtensionContext, false>(
            library_ref, ext_test::TOY_NULL_FACTORY_NAME)),
        std::runtime_error);
    // Same through the deleter-pinned mode load_modules_directory uses.
    BOOST_CHECK_THROW(
        (ext::create_object_from_library<ext::ExtensionContext, true>(
            library_ref, ext_test::TOY_NULL_FACTORY_NAME)),
        std::runtime_error);
}

// The directory pipeline turns the same broken factory into a per-file error
// slot (module skipped, neighbours unaffected) — no crash, no null slot
// sneaking past as a success.
BOOST_AUTO_TEST_CASE(directory_pipeline_records_null_factory_as_error) {
    namespace ext = extension;

    std::vector<std::optional<std::string>> errors;
    auto loaded = ext::load_modules_directory(
        TOY_EXTENSION_DIR, ext::is_likely_dynamic_library,
        ext::same_tag_always{ext_test::TOY_NULL_FACTORY_NAME}, errors);

    BOOST_REQUIRE_EQUAL(loaded.size(), 1u);
    BOOST_CHECK(loaded[0] == nullptr);
    BOOST_REQUIRE_EQUAL(errors.size(), 1u);
    BOOST_REQUIRE(errors[0].has_value());
    BOOST_CHECK_NE(errors[0]->find("returned null"), std::string::npos);
}

// Full directory pipeline: load + verify yields exactly the one good extension,
// correctly ordered.
BOOST_AUTO_TEST_CASE(directory_pipeline_loads_and_verifies_toy) {
    namespace ext = extension;

    std::vector<std::optional<std::string>> errors;
    auto loaded = ext::load_modules_directory(
        TOY_EXTENSION_DIR, ext::is_likely_dynamic_library,
        ext::same_tag_always{ext_test::TOY_EXTENSION_FACTORY_NAME}, errors);

    BOOST_REQUIRE_EQUAL(loaded.size(), 1u);
    BOOST_CHECK(loaded[0] != nullptr);
    BOOST_REQUIRE_EQUAL(errors.size(), 1u);
    BOOST_CHECK(!errors[0].has_value()); // the one slot loaded fine

    auto verified = ext::verify_after_loaded(loaded, errors);
    BOOST_REQUIRE_EQUAL(verified.size(), 1u);
    BOOST_CHECK(verified[0]->name() == ext_test::TOY_EXTENSION_NAME);
    BOOST_CHECK_EQUAL(verified[0]->priority(), ext_test::TOY_EXTENSION_PRIORITY);
}

// The errors-discarding convenience overload must behave like the full one.
BOOST_AUTO_TEST_CASE(directory_convenience_overload_loads_toy) {
    namespace ext = extension;
    auto loaded = ext::load_modules_directory(
        TOY_EXTENSION_DIR, ext::is_likely_dynamic_library,
        ext::same_tag_always{ext_test::TOY_EXTENSION_FACTORY_NAME});
    BOOST_REQUIRE_EQUAL(loaded.size(), 1u);
    BOOST_CHECK(loaded[0] != nullptr);
    BOOST_CHECK(loaded[0]->name() == ext_test::TOY_EXTENSION_NAME);
}

// One-shot load_and_verify_directory: a directory in, a ready-to-use, verified
// and priority-sorted context list out. Exercises the full composed pipeline end
// to end (scan -> load -> deleter-pin -> bind -> filter -> stable sort) and reads
// the metadata of the single survivor.
BOOST_AUTO_TEST_CASE(load_and_verify_directory_loads_sorts_and_verifies_toy) {
    namespace ext = extension;

    std::vector<std::optional<std::string>> errors;
    auto verified = ext::load_and_verify_directory(
        TOY_EXTENSION_DIR, ext::is_likely_dynamic_library,
        ext::same_tag_always{ext_test::TOY_EXTENSION_FACTORY_NAME}, errors);

    // Exactly one module, loaded and verified (no nulls survive verification).
    BOOST_REQUIRE_EQUAL(verified.size(), 1u);
    BOOST_CHECK(verified[0] != nullptr);
    BOOST_CHECK(verified[0]->get_library_ref() != nullptr); // bound by the loader

    // Metadata read through the abstract interface.
    BOOST_CHECK_EQUAL(verified[0]->abi_version(), ext_test::TOY_EXTENSION_ABI_VERSION);
    BOOST_CHECK(verified[0]->name() == ext_test::TOY_EXTENSION_NAME);
    BOOST_CHECK_EQUAL(verified[0]->priority(), ext_test::TOY_EXTENSION_PRIORITY);

    // The parallel errors vector has one slot, and the successful load is nullopt.
    BOOST_REQUIRE_EQUAL(errors.size(), 1u);
    BOOST_CHECK(!errors[0].has_value());
}

// The errors-discarding convenience overload of load_and_verify_directory must
// behave like the full one.
BOOST_AUTO_TEST_CASE(load_and_verify_directory_convenience_overload_loads_toy) {
    namespace ext = extension;
    auto verified = ext::load_and_verify_directory(
        TOY_EXTENSION_DIR, ext::is_likely_dynamic_library,
        ext::same_tag_always{ext_test::TOY_EXTENSION_FACTORY_NAME});
    BOOST_REQUIRE_EQUAL(verified.size(), 1u);
    BOOST_CHECK(verified[0]->name() == ext_test::TOY_EXTENSION_NAME);
}

// Full end-to-end flow over a real DSO through the ExtensionDispatcher:
// load_directory (default import_directory == load_and_verify_directory) builds
// the name index -> dispatch by key -> create_product from the dispatched
// context. The dispatched base pointer is used to mint a distinct Product
// (ToyProduct), whose compute() proves the object is real and its vtable (in the
// .so) was reached safely.
BOOST_AUTO_TEST_CASE(dispatcher_load_then_dispatch_then_create_end_to_end) {
    namespace ext = extension;
    ext::ExtensionDispatcher d;
    std::size_t added = d.load_directory(
        TOY_EXTENSION_DIR, ext::is_likely_dynamic_library,
        ext::same_tag_always{ext_test::TOY_EXTENSION_FACTORY_NAME});
    BOOST_REQUIRE_EQUAL(added, 1u);
    BOOST_CHECK_EQUAL(d.size(), 1u);

    // The name index was built from the loaded context.
    BOOST_CHECK(d.contains(ext_test::TOY_EXTENSION_NAME));
    BOOST_CHECK(d.find(ext_test::TOY_EXTENSION_NAME) != nullptr);

    // Dispatch by name (default matcher) -> base pointer.
    auto chosen = d.dispatch(std::string_view(ext_test::TOY_EXTENSION_NAME));
    BOOST_REQUIRE(chosen != nullptr);
    BOOST_CHECK(chosen->name() == ext_test::TOY_EXTENSION_NAME);

    // Create the real product from the dispatched context's bound library.
    auto product = ext::create_product<ext_test::ToyProduct>(
        chosen, ext_test::TOY_PRODUCT_FACTORY_NAME);
    BOOST_REQUIRE(product != nullptr);
    BOOST_CHECK_EQUAL(product->compute(), ext_test::TOY_PRODUCT_VALUE);

    // A key nothing handles dispatches to null (not a throw).
    BOOST_CHECK(d.dispatch(std::string_view("no_such_extension")) == nullptr);
}

// product_factory resolves the alias ONCE (in the ctor) and create() only invokes
// the cached function pointer — no symbol resolution per Product. Demonstrated by
// minting many products from one factory and checking each is a real, distinct
// ToyProduct whose vtable (in the .so) is reached safely.
BOOST_AUTO_TEST_CASE(product_factory_creates_many_products_without_reimport) {
    namespace ext = extension;
    auto library_ref = ext::get_library_ref(toy_extension_path());

    ext::product_factory<ext_test::ToyProduct> factory(
        library_ref, ext_test::TOY_PRODUCT_FACTORY_NAME);

    constexpr int N = 5;
    std::vector<std::shared_ptr<ext_test::ToyProduct>> products;
    for (int i = 0; i < N; ++i) {
        products.push_back(factory.create());
    }
    BOOST_REQUIRE_EQUAL(products.size(), N);
    for (const auto& p : products) {
        BOOST_REQUIRE(p != nullptr);
        BOOST_CHECK_EQUAL(p->compute(), ext_test::TOY_PRODUCT_VALUE);
    }
    // Each create() yields a distinct object.
    BOOST_CHECK(products[0] != products[1]);
    BOOST_CHECK(products[0] != products[N - 1]);
}

// Default mode (deleter-pinned): a Product keeps the library mapped via its own
// captured ref, so it outlives the factory safely.
BOOST_AUTO_TEST_CASE(product_factory_default_product_survives_factory_destruction) {
    namespace ext = extension;
    std::shared_ptr<ext_test::ToyProduct> product;
    {
        auto library_ref = ext::get_library_ref(toy_extension_path());
        ext::product_factory<ext_test::ToyProduct> factory(
            library_ref, ext_test::TOY_PRODUCT_FACTORY_NAME);
        product = factory.create();
        BOOST_REQUIRE(product != nullptr);
    } // factory + library_ref destroyed here; product still holds a library ref
    BOOST_CHECK_EQUAL(product->compute(), ext_test::TOY_PRODUCT_VALUE);
}

// ---- admission gate: the toolchain-fingerprint magic block -------------------

// A module whose magic block fingerprints a different execution context is
// refused inside get_library_ref — before any alias is resolved and before any
// code in the module runs — with a diagnostic naming both contexts and the
// remedy. This is the gate that turns "same toolchain" from a convention into
// a checked fact (see extension_framework/plugin_magic.hpp).
BOOST_AUTO_TEST_CASE(module_with_wrong_toolchain_fingerprint_is_rejected) {
    auto path = bad_magic_path();
    BOOST_REQUIRE(std::filesystem::exists(path));
    try {
        (void)extension::get_library_ref(path);
        BOOST_FAIL("expected the bad-fingerprint module to be rejected");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_CHECK_NE(what.find("toolchain mismatch"), std::string::npos);
        BOOST_CHECK_NE(what.find("rebuild the plugin"), std::string::npos);
    }
}

// A module without a magic block at all is the legacy shape. Under the
// same-execution-context strategy there is no acceptable unmarked module
// (host and plugins are always rebuilt together), so it is rejected with the
// diagnostic that names the missing macro.
BOOST_AUTO_TEST_CASE(module_without_magic_block_is_rejected) {
    auto path = legacy_module_path();
    BOOST_REQUIRE(std::filesystem::exists(path));
    try {
        (void)extension::get_library_ref(path);
        BOOST_FAIL("expected the legacy module to be rejected");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_CHECK_NE(what.find("no plugin magic block"), std::string::npos);
        BOOST_CHECK_NE(what.find("SIMPLEX_EXPORT_PLUGIN_MAGIC"), std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()
