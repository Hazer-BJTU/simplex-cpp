#define BOOST_TEST_MODULE LoopHookInterface

#include "loop/events.hpp"
#include "loop/hook_interface.hpp"

#include <boost/test/unit_test.hpp>

#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

/** Small stateful hook exercising the same binding path as a built-in hook. */
class CountingHook final : public loop::LoopHookInterface {
public:
    explicit CountingHook(int& calls) : calls_(calls) {
    }

    std::string_view name() const noexcept override {
        return "counting";
    }

protected:
    Subscriptions subscribe(eventbus::EventBus& bus) override {
        Subscriptions subscriptions;
        subscriptions.emplace_back(bus.subscribe<loop::BeforeInput>(
            [this](const loop::BeforeInput& event) {
                ++calls_;
                event.input.role = "user";
            }));
        return subscriptions;
    }

private:
    int& calls_;
};

/** Throws after a successful registration to check partial-bind cleanup. */
class FailingHook final : public loop::LoopHookInterface {
public:
    std::string_view name() const noexcept override {
        return "failing";
    }

protected:
    Subscriptions subscribe(eventbus::EventBus& bus) override {
        Subscriptions subscriptions;
        subscriptions.emplace_back(bus.subscribe<loop::BeforeInput>(
            [](const loop::BeforeInput&) {}));
        throw std::runtime_error("bind failed");
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(binding_owns_hook_and_disconnects_on_destruction) {
    eventbus::EventBus bus;
    int calls = 0;
    auto hook = std::make_shared<CountingHook>(calls);
    std::weak_ptr<CountingHook> weak = hook;

    {
        auto binding = loop::LoopHookInterface::attach(hook, bus);
        hook.reset();
        BOOST_TEST(!weak.expired());
        BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 1U);

        model_io::MessageItem input;
        bus.publish(loop::BeforeInput{input});
        BOOST_TEST(calls == 1);
        BOOST_TEST(input.role == "user");
    }

    BOOST_TEST(weak.expired());
    BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 0U);
}

BOOST_AUTO_TEST_CASE(move_assignment_disconnects_the_previous_hook) {
    eventbus::EventBus bus;
    int old_calls = 0;
    int new_calls = 0;
    auto old_hook = std::make_shared<CountingHook>(old_calls);
    auto new_hook = std::make_shared<CountingHook>(new_calls);
    std::weak_ptr<CountingHook> old_weak = old_hook;

    auto old_binding = loop::LoopHookInterface::attach(old_hook, bus);
    auto new_binding = loop::LoopHookInterface::attach(new_hook, bus);
    old_hook.reset();
    old_binding = std::move(new_binding);

    BOOST_TEST(old_weak.expired());
    BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 1U);
    model_io::MessageItem input;
    bus.publish(loop::BeforeInput{input});
    BOOST_TEST(old_calls == 0);
    BOOST_TEST(new_calls == 1);
}

BOOST_AUTO_TEST_CASE(null_and_partial_bind_are_rejected_cleanly) {
    eventbus::EventBus bus;
    BOOST_CHECK_THROW(([&] {
        auto binding = loop::LoopHookInterface::attach(nullptr, bus);
    }()), std::invalid_argument);
    BOOST_CHECK_THROW(([&] {
        auto binding = loop::LoopHookInterface::attach(
            std::make_shared<FailingHook>(), bus);
    }()), std::runtime_error);
    BOOST_TEST(bus.subscriber_count<loop::BeforeInput>() == 0U);
}
