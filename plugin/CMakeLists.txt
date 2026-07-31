# =============================================================================
# plugin — module
#
# A generic, domain-agnostic plugin framework: the reusable base interface,
# registry, dispatcher, and manager that every plugin domain in the project can
# share. Header-only (all templates), exposed as the `plugin_iface` INTERFACE
# target so a consumer links it once and gets the include path.
#
# It is a sibling of `indextools/`. `indextools/lang_plugin.hpp` derives
# `LangPlugin` from `plugin::Plugin<LangAnalyze>`, and `LangPluginManager`
# composes a `plugin::PluginManager` / `PluginDispatcher` / `PluginRegistry`.
# Future domains (e.g. a command registry) can reuse the same machinery with a
# different product type and dispatch policy.
#
# `boost_dll_iface` (defined in the top-level CMakeLists.txt) carries
# BOOST_DLL_USE_STD_FS + Boost::filesystem + ${CMAKE_DL_LIBS}, so the manager's
# Boost.DLL usage and the SIMPLEX_PLUGIN_ALIAS export resolve cleanly.
#
# Header management is interface-target-only (the project convention): there are
# no directory-scope include_directories() calls here. Consumers link
# `plugin_iface` and get `plugin/include` transitively. Exported headers live
# under include/plugin/ and are included as "plugin/<name>.hpp".
# =============================================================================

add_library(plugin_iface INTERFACE)
target_include_directories(plugin_iface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(plugin_iface INTERFACE boost_dll_iface)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
