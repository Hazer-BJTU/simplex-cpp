#pragma once

/**
 * @file toy_extension_spec.hpp
 * @brief Shared constants for the toy extension used by the dynamic-loading test.
 *
 * The toy domain is a throwaway ExtensionContext implementation with its own ABI
 * constant, factory alias name, name, and priority — none of which the
 * `extension::` framework knows about. test_extensions_dynamic loads
 * libtoyextension.so through the generic pipeline exactly the way a real host
 * would, demonstrating that the framework is reusable for any concrete context.
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

} // namespace ext_test
