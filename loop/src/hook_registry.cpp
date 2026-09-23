#include "loop/hook_registry.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace loop {

LoopHookRegistry::LoopHookRegistry(eventbus::EventBus& bus) noexcept
    : bus_(&bus) {
}

void LoopHookRegistry::add(HookPtr hook) {
    if (!hook) {
        throw std::invalid_argument("LoopHookRegistry::add received a null hook");
    }

    std::string name(hook->name());
    if (name.empty()) {
        throw std::invalid_argument("LoopHookRegistry::add received an unnamed hook");
    }
    if (contains(name)) {
        throw std::runtime_error("loop hook already registered: " + name);
    }

    LoopHookBinding binding = LoopHookInterface::attach(hook, *bus_);
    entries_.push_back(Entry{
        std::move(name),
        std::move(binding),
    });
}

bool LoopHookRegistry::set(HookPtr hook) {
    if (!hook) {
        throw std::invalid_argument("LoopHookRegistry::set received a null hook");
    }

    std::string name(hook->name());
    if (name.empty()) {
        throw std::invalid_argument("LoopHookRegistry::set received an unnamed hook");
    }

    auto existing = find(name);
    if (existing == entries_.end()) {
        add(std::move(hook));
        return false;
    }

    LoopHookBinding binding = LoopHookInterface::attach(hook, *bus_);
    // Move assignment disconnects old slots before releasing the old hook.
    existing->binding = std::move(binding);
    return true;
}

LoopHookRegistry::HookPtr LoopHookRegistry::get(
    std::string_view name) const noexcept {
    const auto found = find(name);
    return found == entries_.end() ? nullptr : found->binding.hook();
}

bool LoopHookRegistry::contains(std::string_view name) const noexcept {
    return find(name) != entries_.end();
}

bool LoopHookRegistry::remove(std::string_view name) noexcept {
    const auto found = find(name);
    if (found == entries_.end()) {
        return false;
    }
    entries_.erase(found);
    return true;
}

void LoopHookRegistry::clear() noexcept {
    entries_.clear();
}

std::size_t LoopHookRegistry::size() const noexcept {
    return entries_.size();
}

bool LoopHookRegistry::empty() const noexcept {
    return entries_.empty();
}

std::vector<LoopHookRegistry::HookPtr> LoopHookRegistry::get_registered() const {
    std::vector<HookPtr> hooks;
    hooks.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        hooks.push_back(entry.binding.hook());
    }
    return hooks;
}

std::vector<LoopHookRegistry::Entry>::iterator LoopHookRegistry::find(
    std::string_view name) noexcept {
    return std::find_if(entries_.begin(), entries_.end(),
                        [name](const Entry& entry) { return entry.name == name; });
}

std::vector<LoopHookRegistry::Entry>::const_iterator LoopHookRegistry::find(
    std::string_view name) const noexcept {
    return std::find_if(entries_.begin(), entries_.end(),
                        [name](const Entry& entry) { return entry.name == name; });
}

} // namespace loop
