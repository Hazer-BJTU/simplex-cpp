#pragma once

/**
 * @file toy_service.hpp
 * @brief A throwaway plugin domain used only to prove the generic framework is
 *        not coupled to languages.
 *
 * The toy domain has its own product interface (`ToyService`), its own ABI
 * constant, and its own factory name — none of which the `plugin::` framework
 * knows about. test_plugin_manager loads `libtoyplugin.so` through the generic
 * `plugin::PluginManager<ToyService>` exactly the way `LangPluginManager` loads
 * language plugins, demonstrating that a second domain reuses the machinery
 * unchanged.
 */

#include <cstdint>
#include <string>

namespace toytest {

/// Product interface the toy plugins produce.
struct ToyService {
    virtual ~ToyService() = default;
    virtual std::string greet() const = 0;
};

/// Toy-domain ABI version (independent of the language ABI).
inline constexpr std::uint32_t TOY_PLUGIN_ABI_VERSION = 1;

/// The BOOST_DLL_ALIAS name the toy plugin exports.
inline constexpr const char* TOY_PLUGIN_FACTORY_NAME = "create_toy_plugin";

} // namespace toytest
