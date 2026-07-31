/**
 * @file toy_plugin.cpp
 * @brief A toy plugin module (libtoyplugin.so) for the generic-framework test.
 *
 * Builds into a dynamically-loaded library that exports `create_toy_plugin` via
 * SIMPLEX_PLUGIN_ALIAS. It is a completely separate domain from the language
 * plugins: a different product (`ToyService`), ABI constant, and factory name —
 * loaded by `plugin::PluginManager<ToyService>` in test_plugin_manager.cpp.
 */

#include "toy_service.hpp"

#include "plugin/plugin.hpp"

#include <boost/dll/alias.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace toytest {

namespace {

class HelloPlugin final : public plugin::Plugin<ToyService> {
public:
    std::uint32_t abi_version() const noexcept override {
        return TOY_PLUGIN_ABI_VERSION;
    }

    std::string_view name() const noexcept override {
        return "Hello";
    }

    std::unique_ptr<ToyService> create() const override {
        // Concrete service whose vtable + dtor live in this .so.
        struct HelloService final : ToyService {
            std::string greet() const override {
                return "hello from toy plugin";
            }
        };
        return std::make_unique<HelloService>();
    }
};

} // namespace

// Erased factory return type (matches the import_alias signature in the test),
// so there is no return-type reinterpretation across the boundary.
std::shared_ptr<plugin::Plugin<ToyService>> create_toy_plugin() {
    return std::make_shared<HelloPlugin>();
}

} // namespace toytest

SIMPLEX_PLUGIN_ALIAS(toytest::create_toy_plugin, create_toy_plugin)
