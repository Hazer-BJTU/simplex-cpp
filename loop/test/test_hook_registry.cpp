#define BOOST_TEST_MODULE LoopHookRegistry

#include "loop/events.hpp"
#include "loop/hook_registry.hpp"

#include <boost/test/unit_test.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/** A named hook that records callback order and can fail during binding. */
class TestHook final : public loop::LoopHookInterface {
public:
    TestHook(std::string name, std::vector<std::string>& calls, bool fail = false)
        : name_(std::move(name)), calls_(calls), fail_(fail) {
    }

    std::string_view name() const noexcept override {
        return name_;
    }

protected:
    Subscriptions subscribe(eventbus::EventBus& bus) override {
        Subscriptions subscriptions;
        subscriptions.emplace_back(bus.subscribe<loop::BeforeInput>(
            [this](const loop::BeforeInput&) {
                calls_.push_back(name_);
            }));
        if (fail_) {
            throw std::runtime_error("hook binding failed");
        }
        return subscriptions;
    }

private:
    std::string name_;
    std::vector<std::string>& calls_;
    bool fail_;
};

void publish_input(eventbus::EventBus& bus) {
    model_io::MessageItem input;
    bus.publish(loop::BeforeInput{input});
}

} // namespace

BOOST_AUTO_TEST_CASE(add_get_remove_and_clear_manage_subscriptions) {
    eventbus::EventBus bus;
    loop::LoopHookRegistry registry(bus);
    std::vector<std::string> calls;
    auto first = std::make_shared<TestHook>("first", calls);
    auto second = std::make_shared<TestHook>("second", calls);
    std::weak_ptr<TestHook> first_weak = first;

    registry.add(first);
    registry.add(second);
    first.reset();
    BOOST_TEST(registry.size() == 2U);
    BOOST_TEST(registry.contains("first"));
    BOOST_TEST(registry.get("first") == first_weak.lock());
    BOOST_TEST(registry.get("missing") == nullptr);
    BOOST_TEST(registry.get_registered().size() == 2U);

    publish_input(bus);
    BOOST_TEST(calls == std::vector<std::string>({"first", "second"}));

    BOOST_TEST(registry.remove("first"));
    BOOST_TEST(!registry.remove("first"));
    BOOST_TEST(first_weak.expired());
    calls.clear();
    publish_input(bus);
    BOOST_TEST(calls == std::vector<std::string>({"second"}));

    registry.clear();
    BOOST_TEST(registry.empty());
    BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 0U);
}

BOOST_AUTO_TEST_CASE(set_replaces_by_name_and_failed_bind_keeps_old_hook) {
    eventbus::EventBus bus;
    loop::LoopHookRegistry registry(bus);
    std::vector<std::string> calls;
    auto old = std::make_shared<TestHook>("same", calls);
    std::weak_ptr<TestHook> old_weak = old;
    registry.add(old);
    old.reset();

    BOOST_CHECK_THROW(registry.set(std::make_shared<TestHook>(
        "same", calls, true)), std::runtime_error);
    BOOST_TEST(registry.size() == 1U);
    BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 1U);
    publish_input(bus);
    BOOST_TEST(calls == std::vector<std::string>({"same"}));

    auto replacement = std::make_shared<TestHook>("same", calls);
    BOOST_TEST(registry.set(replacement));
    BOOST_TEST(old_weak.expired());
    BOOST_TEST(registry.get("same") == replacement);
    BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 1U);

    auto added = std::make_shared<TestHook>("new", calls);
    BOOST_TEST(!registry.set(added));
    BOOST_TEST(registry.size() == 2U);
}

BOOST_AUTO_TEST_CASE(replacement_appends_after_other_hooks) {
    eventbus::EventBus bus;
    loop::LoopHookRegistry registry(bus);
    std::vector<std::string> calls;
    registry.add(std::make_shared<TestHook>("first", calls));
    registry.add(std::make_shared<TestHook>("second", calls));
    BOOST_TEST(registry.set(std::make_shared<TestHook>("first", calls)));

    publish_input(bus);
    BOOST_TEST(calls == std::vector<std::string>({"second", "first"}));
}

BOOST_AUTO_TEST_CASE(invalid_registration_does_not_change_registry) {
    eventbus::EventBus bus;
    loop::LoopHookRegistry registry(bus);
    std::vector<std::string> calls;
    registry.add(std::make_shared<TestHook>("valid", calls));

    BOOST_CHECK_THROW(registry.add(nullptr), std::invalid_argument);
    BOOST_CHECK_THROW(registry.set(nullptr), std::invalid_argument);
    BOOST_CHECK_THROW(registry.add(std::make_shared<TestHook>("", calls)),
                      std::invalid_argument);
    BOOST_CHECK_THROW(registry.add(std::make_shared<TestHook>("valid", calls)),
                      std::runtime_error);
    BOOST_TEST(registry.size() == 1U);
    BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 1U);
}
