#define BOOST_TEST_MODULE PluginFrameworkDispatcherTests
#include <boost/test/unit_test.hpp>

#include "plugin/dispatcher.hpp"
#include "plugin/plugin.hpp"

#include <memory>
#include <string>
#include <string_view>

using namespace plugin;

namespace {

// Minimal in-memory product + plugin.
struct FakeProduct {
    virtual ~FakeProduct() = default;
};

struct FakePlugin : Plugin<FakeProduct> {
    std::string label;
    int prio = 0;
    FakePlugin(std::string l, int p) : label(std::move(l)), prio(p) {}
    std::uint32_t abi_version() const noexcept override { return 1; }
    std::string_view name() const noexcept override { return label; }
    int priority() const noexcept override { return prio; }
    std::unique_ptr<FakeProduct> create() const override {
        return std::make_unique<FakeProduct>();
    }
};

// --- Selector strategy A: exact-name match --------------------------------
// Payload is the name the plugin answers to; the key is the requested name.
struct ExactNameSelector {
    bool operator()(const std::string& payload, const std::string& key) const {
        return payload == key;
    }
};
using NameDispatcher =
    PluginDispatcher<FakeProduct, std::string, ExactNameSelector, std::string>;

// --- Selector strategy B: catch-all (payload-free) ------------------------
// Payload defaults to std::monostate; matches every key. This is a different
// dispatch shape than exact-name, proving the dispatcher is policy-driven.
struct AlwaysSelector {
    bool operator()(const std::monostate&, const std::string&) const { return true; }
};
using CatchAllDispatcher =
    PluginDispatcher<FakeProduct, std::string, AlwaysSelector>;

} // namespace

BOOST_AUTO_TEST_SUITE(DispatcherSuite)

// Lazy sort (no explicit flush) still yields correct selection.
BOOST_AUTO_TEST_CASE(selects_by_exact_name_payload) {
    NameDispatcher d;
    d.add(std::make_shared<FakePlugin>("alpha", 0), "alpha");
    d.add(std::make_shared<FakePlugin>("beta", 0), "beta");

    BOOST_CHECK(d.select("alpha")->name() == "alpha");
    BOOST_CHECK(d.select("beta")->name() == "beta");
    BOOST_CHECK(d.select("gamma") == nullptr);
    BOOST_CHECK(d.any_match("alpha"));
    BOOST_CHECK(!d.any_match("gamma"));
}

BOOST_AUTO_TEST_CASE(higher_priority_wins_over_insertion_order) {
    NameDispatcher d;
    d.add(std::make_shared<FakePlugin>("dedicated", 0), "shared");
    d.add(std::make_shared<FakePlugin>("override", 10), "shared");
    d.flush(); // both claim "shared"; the priority-10 entry routes first
    BOOST_CHECK(d.select("shared")->name() == "override");
}

BOOST_AUTO_TEST_CASE(equal_priority_preserves_insertion_order) {
    NameDispatcher d;
    d.add(std::make_shared<FakePlugin>("first", 5), "k");
    d.add(std::make_shared<FakePlugin>("second", 5), "k");
    d.flush();
    BOOST_CHECK(d.select("k")->name() == "first");
}

BOOST_AUTO_TEST_CASE(catch_all_strategy_matches_any_key) {
    CatchAllDispatcher d;
    d.add(std::make_shared<FakePlugin>("fallback", -1000));
    BOOST_CHECK(d.any_match("anything"));
    BOOST_CHECK(d.any_match("totally-unrelated"));
    BOOST_CHECK(d.select("whatever")->name() == "fallback");
}

BOOST_AUTO_TEST_CASE(find_locates_by_plugin_name) {
    NameDispatcher d;
    auto p = std::make_shared<FakePlugin>("solo", 0);
    d.add(p, "answers-to-solo"); // payload != name, on purpose
    BOOST_CHECK(d.find("solo") == p);
    BOOST_CHECK(d.find("answers-to-solo") == nullptr); // find is by name(), not payload
    BOOST_CHECK_EQUAL(d.size(), 1u);
}

BOOST_AUTO_TEST_CASE(empty_dispatcher_matches_nothing) {
    NameDispatcher d;
    BOOST_CHECK(d.empty());
    BOOST_CHECK(d.select("x") == nullptr);
    BOOST_CHECK(!d.any_match("x"));
}

BOOST_AUTO_TEST_CASE(clear_removes_entries) {
    NameDispatcher d;
    d.add(std::make_shared<FakePlugin>("a", 0), "a");
    BOOST_REQUIRE_EQUAL(d.size(), 1u);
    d.clear();
    BOOST_CHECK_EQUAL(d.size(), 0u);
    BOOST_CHECK(d.select("a") == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
