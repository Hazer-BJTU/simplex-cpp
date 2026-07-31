#define BOOST_TEST_MODULE PluginFrameworkRegistryTests
#include <boost/test/unit_test.hpp>

#include "plugin/plugin.hpp"
#include "plugin/registry.hpp"

#include <memory>
#include <string>
#include <string_view>

using namespace plugin;

namespace {

// Minimal in-memory product + plugin (no .so needed to test the registry).
struct FakeProduct {
    virtual ~FakeProduct() = default;
};

struct FakePlugin : Plugin<FakeProduct> {
    std::string n;
    explicit FakePlugin(std::string s) : n(std::move(s)) {}
    std::uint32_t abi_version() const noexcept override { return 1; }
    std::string_view name() const noexcept override { return n; }
    std::unique_ptr<FakeProduct> create() const override {
        return std::make_unique<FakeProduct>();
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(RegistrySuite)

BOOST_AUTO_TEST_CASE(register_find_contains) {
    PluginRegistry<FakeProduct> r;
    auto p = std::make_shared<FakePlugin>("alpha");
    BOOST_CHECK(r.register_plugin(p));

    BOOST_CHECK(r.contains("alpha"));
    BOOST_CHECK(r.find("alpha") == p);
    BOOST_CHECK(r.find("missing") == nullptr);
    BOOST_CHECK_EQUAL(r.size(), 1u);
    BOOST_CHECK(!r.empty());
}

BOOST_AUTO_TEST_CASE(duplicate_name_is_rejected) {
    PluginRegistry<FakeProduct> r;
    BOOST_CHECK(r.register_plugin(std::make_shared<FakePlugin>("dup")));
    // A second plugin under the same name is refused, not silently overwritten.
    BOOST_CHECK(!r.register_plugin(std::make_shared<FakePlugin>("dup")));
    BOOST_CHECK_EQUAL(r.size(), 1u);
}

BOOST_AUTO_TEST_CASE(null_plugin_is_rejected) {
    PluginRegistry<FakeProduct> r;
    BOOST_CHECK(!r.register_plugin(nullptr));
    BOOST_CHECK_EQUAL(r.size(), 0u);
    BOOST_CHECK(r.empty());
}

BOOST_AUTO_TEST_CASE(all_snapshots_registered_plugins) {
    PluginRegistry<FakeProduct> r;
    r.register_plugin(std::make_shared<FakePlugin>("a"));
    r.register_plugin(std::make_shared<FakePlugin>("b"));
    r.register_plugin(std::make_shared<FakePlugin>("c"));

    auto snapshot = r.all();
    BOOST_CHECK_EQUAL(snapshot.size(), 3u);
}

BOOST_AUTO_TEST_SUITE_END()
