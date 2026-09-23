// Deliberately malformed modules used only by loader rejection tests.
#include "tools/extensions/plugin.hpp"
#include "extensions/plugin_magic.hpp"
#include <boost/dll/alias.hpp>
#include <stdexcept>

namespace fixture {

#ifdef SIMPLEX_TEST_WRONG_CONTEXT
using ContextBase = extension::ExtensionContext;
#else
using ContextBase = tools::extensions::ToolSetExtensionContext;
#endif

class Context final : public ContextBase {
public:
    std::uint32_t abi_version() const noexcept override {
#ifdef SIMPLEX_TEST_BAD_ABI
        return 999;
#else
        return tools::extensions::kAbiVersion;
#endif
    }
    std::string_view name() const noexcept override {
        return "noop_toolset";
    }
};

class Product final : public tools::ToolSet {
public:
    std::string_view name() const noexcept override {
        return "wrong_product_name";
    }
    std::vector<model_io::Invocable> get_tools() const override {
        return {};
    }
    ToolHandle dispatch(const model_io::InvokeQuery&) const override {
        return nullptr;
    }
};

std::unique_ptr<extension::ExtensionContext> context() {
    return std::make_unique<Context>();
}

std::unique_ptr<tools::ToolSet> create(const tools::extensions::ToolSetConfig&) {
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

BOOST_DLL_ALIAS(fixture::context, create_toolset_plugin)
#ifndef SIMPLEX_TEST_MISSING_FACTORY
BOOST_DLL_ALIAS(fixture::create, create_toolset)
#endif
#ifndef SIMPLEX_TEST_NO_MAGIC
SIMPLEX_EXPORT_PLUGIN_MAGIC;
#endif
