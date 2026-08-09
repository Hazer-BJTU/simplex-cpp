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

BOOST_AUTO_TEST_SUITE_END()
