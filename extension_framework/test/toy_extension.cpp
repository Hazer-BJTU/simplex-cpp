/**
 * @file toy_extension.cpp
 * @brief A toy extension module (libtoyextension.so) for the dynamic-load test.
 *
 * Builds into a dynamically-loaded library that exports two factory aliases:
 *
 *   - `create_toy_extension` — mints the identity ExtensionContext (whose vtable
 *     + destructor live in this .so — exactly the lifetime situation the
 *     framework's library-binding contract is designed to keep safe).
 *   - `create_toy_product`   — mints a distinct ToyProduct, exercising the
 *     `create_product<Product>` path over a real DSO with a type that is not an
 *     ExtensionContext.
 *
 * Both factories' return types match their `import_alias` signatures exactly (no
 * reinterpretation across the boundary).
 */

#include "toy_extension_spec.hpp"

#include "extension_framework/extensions.hpp"

#include <boost/dll/alias.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace ext_test {

namespace {

class ToyExtension final : public extension::ExtensionContext {
public:
    std::uint32_t abi_version() const noexcept override {
        return TOY_EXTENSION_ABI_VERSION;
    }

    std::string_view name() const noexcept override {
        return TOY_EXTENSION_NAME;
    }

    long priority() const noexcept override {
        return TOY_EXTENSION_PRIORITY;
    }
};

/// Concrete ToyProduct: returns the shared TOY_PRODUCT_VALUE.
class ConcreteToyProduct final : public ToyProduct {
public:
    int compute() const noexcept override {
        return TOY_PRODUCT_VALUE;
    }
};

} // namespace

// Identity-context factory imported by the host via
// import_alias<unique_ptr<ExtensionContext>()>.
std::unique_ptr<extension::ExtensionContext> create_toy_extension() {
    return std::make_unique<ToyExtension>();
}

// Product factory imported by the host via import_alias<unique_ptr<ToyProduct>()>.
std::unique_ptr<ToyProduct> create_toy_product() {
    return std::make_unique<ConcreteToyProduct>();
}

} // namespace ext_test

BOOST_DLL_ALIAS(ext_test::create_toy_extension, create_toy_extension)
BOOST_DLL_ALIAS(ext_test::create_toy_product, create_toy_product)
