#include "loop/hook_interface.hpp"

#include <stdexcept>
#include <utility>

namespace loop {

LoopHookBinding::LoopHookBinding(
    std::shared_ptr<LoopHookInterface> hook,
    Subscriptions subscriptions) noexcept
    : hook_(std::move(hook)),
      subscriptions_(std::move(subscriptions)) {
}

LoopHookBinding& LoopHookBinding::operator=(LoopHookBinding&& other) noexcept {
    if (this != &other) {
        // Disconnect the old callbacks before releasing their hook instance.
        subscriptions_.clear();
        hook_ = std::move(other.hook_);
        subscriptions_ = std::move(other.subscriptions_);
    }
    return *this;
}

std::shared_ptr<LoopHookInterface> LoopHookBinding::hook() const noexcept {
    return hook_;
}

LoopHookInterface::~LoopHookInterface() = default;

LoopHookBinding LoopHookInterface::attach(
    std::shared_ptr<LoopHookInterface> hook,
    eventbus::EventBus& bus) {
    if (!hook) {
        throw std::invalid_argument("loop hook must not be null");
    }

    Subscriptions subscriptions = hook->subscribe(bus);
    return LoopHookBinding(std::move(hook), std::move(subscriptions));
}

} // namespace loop
