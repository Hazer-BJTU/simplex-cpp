# =============================================================================
# extension_framework — module
#
# The generic, domain-agnostic *extension* framework: dynamic-library discovery,
# loading, verification, and the ExtensionContext base interface. It is the
# successor to (and eventual replacement for) the older `plugin/` framework,
# collapsing the loading pipeline into a single header, extensions.hpp.
#
# Header-only (templates + a few inline free functions), exposed as the
# `extension_framework_iface` INTERFACE target so a consumer links it once and
# gets the include path plus every transitive dependency.
#
# Dependencies (all declared at the top level or in sibling modules, so this
# module adds no find_package() of its own):
#   - boost_dll_iface        (top-level): BOOST_DLL_USE_STD_FS + Boost::filesystem
#                              + ${CMAKE_DL_LIBS}. Needed for the shared_library
#                              / import_alias usage and for BOOST_DLL_ALIAS in
#                              any extension module built against this target.
#   - logging_lib            (utils/logging): verify_after_loaded() calls
#                              logging::Logger::info/warning, whose definitions
#                              live in logging/src/logger.cpp. Linked PUBLIC so a
#                              consumer of this header resolves them transparently.
#   - simplex_thirdparty_iface (top-level): carries the vendored nlohmann/json
#                              include dir used by ExtensionContext::extras().
#
# Header management is interface-target-only (the project convention): there are
# no directory-scope include_directories() calls here. Consumers link
# `extension_framework_iface` and get `extension_framework/include` transitively.
# Exported headers live under include/extension_framework/ and are included as
# "extension_framework/<name>.hpp".
# =============================================================================

add_library(extension_framework_iface INTERFACE)
target_include_directories(extension_framework_iface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(extension_framework_iface INTERFACE
    boost_dll_iface
    logging_lib
    simplex_thirdparty_iface)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
