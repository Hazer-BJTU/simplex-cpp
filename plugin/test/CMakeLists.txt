# Unit tests for the generic plugin framework.
#
# test_plugin_registry / test_plugin_dispatcher are pure in-memory (header-only
# framework, no .so). test_plugin_manager drives the generic PluginManager over
# a toy dynamic library (libtoyplugin.so) — a non-language domain, proving the
# framework is reusable. Each test_*.cpp is one executable (Boost.Test static
# linking needs one BOOST_TEST_MODULE/main per binary).

set(_plugin_test_opts
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# --- in-memory tests ---------------------------------------------------------
add_executable(test_plugin_registry test_plugin_registry.cpp)
target_compile_options(test_plugin_registry PRIVATE ${_plugin_test_opts})
target_link_libraries(test_plugin_registry PRIVATE plugin_iface Boost::unit_test_framework)
add_test(NAME test_plugin_registry COMMAND test_plugin_registry)

add_executable(test_plugin_dispatcher test_plugin_dispatcher.cpp)
target_compile_options(test_plugin_dispatcher PRIVATE ${_plugin_test_opts})
target_link_libraries(test_plugin_dispatcher PRIVATE plugin_iface Boost::unit_test_framework)
add_test(NAME test_plugin_dispatcher COMMAND test_plugin_dispatcher)

# --- toy DSO for the generic PluginManager load+lifetime test ----------------
# Emitted into bin/test_plugins (NOT bin/plugins) so the real
# LangPluginManager's default-directory scan never sees it and prints a spurious
# "[plugin] skip libtoyplugin: missing symbol 'create_lang_plugin'" line.
# plugin_iface already links boost_dll_iface (BOOST_DLL_USE_STD_FS + filesystem
# + dl), so BOOST_DLL_ALIAS / SIMPLEX_PLUGIN_ALIAS resolve with no extra link.
add_library(toyplugin MODULE toy_plugin.cpp)
target_include_directories(toyplugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(toyplugin PRIVATE ${_plugin_test_opts})
target_link_libraries(toyplugin PRIVATE plugin_iface)
set_target_properties(toyplugin PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/test_plugins")

add_executable(test_plugin_framework_manager test_plugin_manager.cpp)
target_include_directories(test_plugin_framework_manager PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(test_plugin_framework_manager PRIVATE ${_plugin_test_opts})
target_link_libraries(test_plugin_framework_manager PRIVATE plugin_iface Boost::unit_test_framework)
target_compile_definitions(test_plugin_framework_manager PRIVATE
    "TOY_PLUGIN_DIR=\"${CMAKE_BINARY_DIR}/bin/test_plugins\"")
add_dependencies(test_plugin_framework_manager toyplugin)
add_test(NAME test_plugin_framework_manager COMMAND test_plugin_framework_manager)
