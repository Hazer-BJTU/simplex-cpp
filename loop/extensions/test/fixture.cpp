// Deliberately malformed modules used only by loader rejection tests.
#include "loop/extensions/plugin.hpp"
#include "extensions/plugin_magic.hpp"
#include <boost/dll/alias.hpp>
#include <stdexcept>

namespace fixture {

#ifdef SIMPLEX_TEST_WRONG_CONTEXT
using ContextBase = extension::ExtensionContext;
#else
using ContextBase = loop::extensions::LoopHookExtensionContext;
#endif

class Context final : public ContextBase {
public:
    std::uint32_t abi_version() const noexcept override {
#ifdef SIMPLEX_TEST_BAD_ABI
        return 999;
#else
        return loop::extensions::kAbiVersion;
#endif
    }
    std::string_view name() const noexcept override {
        return "noop_hook";
    }
};

class Product final : public loop::LoopHookInterface {
public:
    std::string_view name() const noexcept override {
        return "wrong_product_name";
    }
protected:
    Subscriptions subscribe(eventbus::EventBus&) override {
        return {};
    }
};

std::unique_ptr<extension::ExtensionContext> context() {
    return std::make_unique<Context>();
}

std::unique_ptr<loop::LoopHookInterface> create(const loop::intrinsic::HookConfig&) {
#ifdef SIMPLEX_TEST_NULL_PRODUCT
    return nullptr;
#elif defined(SIMPLEX_TEST_THROW_PRODUCT)
    throw std::runtime_error("factory test failure");
#elif defined(SIMPLEX_TEST_NONSTANDARD_PRODUCT)
    throw 7;
#else
    return std::make_unique<Product>();
#endif
}

} // namespace fixture

BOOST_DLL_ALIAS(fixture::context, create_loop_hook_plugin)
#ifndef SIMPLEX_TEST_MISSING_FACTORY
BOOST_DLL_ALIAS(fixture::create, create_loop_hook)
#endif
#ifndef SIMPLEX_TEST_NO_MAGIC
SIMPLEX_EXPORT_PLUGIN_MAGIC;
#endif
