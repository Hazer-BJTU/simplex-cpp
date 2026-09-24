#include "loop/extensions/plugin.hpp"
#include "extensions/plugin_magic.hpp"

#include <boost/dll/alias.hpp>
#include <stdexcept>

namespace LOAD_FIXTURE_NAMESPACE {

/** Minimal configurable identity used to exercise selection and ordering. */
class Hook final : public loop::LoopHookInterface {
public:
    std::string_view name() const noexcept override {
        return LOAD_FIXTURE_NAME;
    }

protected:
    Subscriptions subscribe(eventbus::EventBus&) override {
        return {};
    }
};

/** Normal descriptor even when the product factory deliberately fails. */
class Context final : public loop::extensions::LoopHookExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return loop::extensions::kAbiVersion;
    }

    std::string_view name() const noexcept override {
        return LOAD_FIXTURE_NAME;
    }
};

std::unique_ptr<extension::ExtensionContext> descriptor() {
    return std::make_unique<Context>();
}

std::unique_ptr<loop::LoopHookInterface> product(const loop::intrinsic::HookConfig&) {
#ifdef LOAD_FAIL_FACTORY
    throw std::runtime_error("unselected product factory was called");
#else
    return std::make_unique<Hook>();
#endif
}

} // namespace LOAD_FIXTURE_NAMESPACE

BOOST_DLL_ALIAS(LOAD_FIXTURE_NAMESPACE::descriptor, create_loop_hook_plugin)
BOOST_DLL_ALIAS(LOAD_FIXTURE_NAMESPACE::product, create_loop_hook)
SIMPLEX_EXPORT_PLUGIN_MAGIC;
