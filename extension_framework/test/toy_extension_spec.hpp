#pragma once

/**
 * @file toy_extension_spec.hpp
 * @brief Shared contract for the toy extension used by the dynamic-loading test.
 *
 * The toy domain is a throwaway ExtensionContext implementation with its own ABI
 * constant, factory alias name, name, and priority — none of which the
 * `extension::` framework knows about. test_extensions_dynamic loads
 * libtoyextension.so through the generic pipeline exactly the way a real host
 * would, demonstrating that the framework is reusable for any concrete context.
 *
 * It also exports a distinct Product type (`ToyProduct`) via a second factory
 * alias, so `create_product<Product>` can be exercised over a real DSO with a
 * type that is NOT an ExtensionContext — the realistic shape of a domain that
 * exports both an identity context and the objects it produces.
 */

#include <cstdint>

namespace ext_test {

/// Toy-domain ABI version (independent of any other domain).
inline constexpr std::uint32_t TOY_EXTENSION_ABI_VERSION = 1;

/// The BOOST_DLL_ALIAS name the toy extension exports.
inline constexpr const char* TOY_EXTENSION_FACTORY_NAME = "create_toy_extension";

/// The name() the toy extension reports.
inline constexpr const char* TOY_EXTENSION_NAME = "Toy";

/// The priority() the toy extension reports (non-default, so sorting is tested).
inline constexpr long TOY_EXTENSION_PRIORITY = 7;

// -----------------------------------------------------------------------------
// ToyProduct — a distinct Product the toy extension can mint.
// ----------------------------------------------------------------------------

/**
 * @brief Abstract product type minted by `create_toy_product`.
 *
 * Lives in this shared header so both the toy .so (which implements it) and the
 * test host (which calls `create_product<ToyProduct>`) see the same vtable
 * layout. Demonstrates that `create_product` / `create_object_from_library` work
 * for any factory-imported type, not just `ExtensionContext`.
 */
class ToyProduct {
public:
    virtual ~ToyProduct() = default;
    /// A deterministic value the host can assert against (TOY_PRODUCT_VALUE).
    virtual int compute() const noexcept = 0;
};

/// The BOOST_DLL_ALIAS name the toy product factory is exported under.
inline constexpr const char* TOY_PRODUCT_FACTORY_NAME = "create_toy_product";

/// The value a ToyProduct's compute() returns (asserted by the test).
inline constexpr int TOY_PRODUCT_VALUE = 42;

} // namespace ext_test
