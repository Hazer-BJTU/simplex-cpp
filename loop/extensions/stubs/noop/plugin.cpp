#include "loop/extensions/plugin.hpp"
#include "loop/events.hpp"
#include "extensions/plugin_magic.hpp"

#include <boost/dll/alias.hpp>

#include <stdexcept>

namespace loop::extensions::noop {

class NoopHook final : public loop::LoopHookInterface {
public:
    std::string_view name() const noexcept override {
        return "noop_hook";
    }
protected:
    Subscriptions subscribe(eventbus::EventBus& bus) override {
        Subscriptions result;
        eventbus::EventBus::ScopedSubscription owned{
            bus.subscribe<loop::RunStarted>([](const loop::RunStarted&) {})};
        result.push_back(std::move(owned));
        return result;
    }
};

class NoopContext final : public loop::extensions::LoopHookExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return loop::extensions::kAbiVersion;
    }
    std::string_view name() const noexcept override {
        return "noop_hook";
    }
};

std::unique_ptr<extension::ExtensionContext> create_loop_hook_plugin() {
    return std::make_unique<NoopContext>();
}

std::unique_ptr<loop::LoopHookInterface> create_loop_hook(
    const loop::intrinsic::HookConfig& config) {
    if (config.name != "noop_hook" || !config.config.empty()) {
        throw std::invalid_argument("noop_hook accepts no options");
    }
    return std::make_unique<NoopHook>();
}

} // namespace loop::extensions::noop

BOOST_DLL_ALIAS(loop::extensions::noop::create_loop_hook_plugin, create_loop_hook_plugin)
BOOST_DLL_ALIAS(loop::extensions::noop::create_loop_hook, create_loop_hook)
SIMPLEX_EXPORT_PLUGIN_MAGIC;
