#define BOOST_TEST_MODULE PluginFrameworkManagerTests
#include <boost/test/unit_test.hpp>

#include "plugin/manager.hpp"
#include "toy_service.hpp"

#include <boost/dll/shared_library.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

using namespace plugin;

#ifndef TOY_PLUGIN_DIR
#error "TOY_PLUGIN_DIR must be defined by the build system"
#endif

namespace {

using ToyProduct = toytest::ToyService;
using ToyPlugin = plugin::Plugin<ToyProduct>;
using ToyManager = PluginManager<ToyProduct>;

// Signature of the toy plugin's exported factory (erased return type, so the
// import_alias template argument exactly matches the symbol's return type).
using ToyFactory = std::shared_ptr<ToyPlugin>();

// Domain loader: import the typed factory alias and invoke it. Captures the
// domain factory name (a constant) — the generic signature only passes the lib.
std::shared_ptr<ToyPlugin> toy_loader(boost::dll::shared_library& lib) {
    auto factory = boost::dll::import_alias<ToyFactory>(
        lib, toytest::TOY_PLUGIN_FACTORY_NAME);
    return factory();
}

// Holds the most-recently-loaded plugin so a test can drive create_instance.
struct Loaded {
    std::shared_ptr<ToyPlugin> plugin;
    std::shared_ptr<boost::dll::shared_library> library;
};

} // namespace

BOOST_AUTO_TEST_SUITE(PluginManagerSuite)

BOOST_AUTO_TEST_CASE(loads_toy_plugin_and_instantiates) {
    ToyManager mgr;
    Loaded loaded;
    std::size_t n = mgr.load_directory<ToyPlugin>(
        TOY_PLUGIN_DIR, toytest::TOY_PLUGIN_FACTORY_NAME,
        toytest::TOY_PLUGIN_ABI_VERSION, toy_loader,
        [&](const LoadResult<ToyPlugin>& r) {
            loaded.plugin = r.plugin;
            loaded.library = r.library;
        });
    BOOST_REQUIRE_EQUAL(n, 1u);
    BOOST_REQUIRE(loaded.plugin);

    auto svc = mgr.create_instance(loaded.plugin, loaded.library);
    BOOST_REQUIRE(svc);
    BOOST_CHECK_EQUAL(svc->greet(), "hello from toy plugin");
}

BOOST_AUTO_TEST_CASE(rejects_plugin_with_wrong_abi_version) {
    // Expecting ABI 9999 means the real (ABI 1) plugin is skipped on load.
    ToyManager mgr;
    std::size_t n = mgr.load_directory<ToyPlugin>(
        TOY_PLUGIN_DIR, toytest::TOY_PLUGIN_FACTORY_NAME, /*abi_version=*/9999,
        toy_loader, [](const LoadResult<ToyPlugin>&) {});
    BOOST_CHECK_EQUAL(n, 0u);
}

// The crux of plugin mode: an instance carries its library in its deleter, so it
// stays fully usable after every other handle to the loaded plugin is dropped —
// and its destructor runs safely while the library is still mapped.
BOOST_AUTO_TEST_CASE(instance_outlives_every_other_handle) {
    ToyManager mgr;
    std::shared_ptr<ToyProduct> svc;
    {
        Loaded loaded; // dropped at the end of this block, along with each LoadResult
        mgr.load_directory<ToyPlugin>(
            TOY_PLUGIN_DIR, toytest::TOY_PLUGIN_FACTORY_NAME,
            toytest::TOY_PLUGIN_ABI_VERSION, toy_loader,
            [&](const LoadResult<ToyPlugin>& r) {
                svc = mgr.create_instance(r.plugin, r.library);
            });
    }
    BOOST_REQUIRE(svc);

    // Calling into plugin code must be safe (vtable lives in the still-loaded .so).
    BOOST_CHECK_EQUAL(svc->greet(), "hello from toy plugin");

    // Destruction must be safe too (~ToyService code is in the .so); the deleter
    // only releases the library reference *after* the object is destroyed.
    svc.reset();
    BOOST_CHECK(!svc);
}

BOOST_AUTO_TEST_SUITE_END()
