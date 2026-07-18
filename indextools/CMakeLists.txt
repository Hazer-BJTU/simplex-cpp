# =============================================================================
# indextools — module
#
# This is a subdirectory module of the top-level `simplex_cpp` project. The
# toolchain, C++ standard, LTO probe, Boost find, the `boost_dll_iface`
# interface target, the build-output directories, and the bundled
# `third_party/` location are all owned by the parent CMakeLists.txt — see the
# top of that file for what lives where and why, and `dependency.md` for the
# per-module dependency inventory.
#
# This file owns only indextools-specific build logic:
#   - the `indextools` host executable
#   - its language plugin modules (via languages/)
#   - its unit tests (via test/)
#
# Path conventions used here and in the child subdirectories (languages/, test/):
#   ${INDEXTOOLS_SOURCE_DIR}        - this directory (indextools/)
#   ${INDEXTOOLS_INCLUDE_DIR}       - indextools/include  (project headers)
#   ${SIMPLEX_THIRDPARTY_DIR}       - top-level third_party/  (set by parent)
#   ${SIMPLEX_THIRDPARTY_DIR}/include  - vendored headers (nlohmann, glob, tree-sitter)
#   ${SIMPLEX_THIRDPARTY_DIR}/libs     - vendored static libs (tree-sitter .a)
# These variables propagate to child scopes added below.
# =============================================================================

set(INDEXTOOLS_SOURCE_DIR  "${CMAKE_CURRENT_SOURCE_DIR}")
set(INDEXTOOLS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# indextools headers are visible to this module's own targets. third_party
# headers come in via simplex_thirdparty_iface; the lib search path likewise.
include_directories(
    "${INDEXTOOLS_INCLUDE_DIR}"
    "${SIMPLEX_THIRDPARTY_DIR}/include"
)
link_directories("${SIMPLEX_THIRDPARTY_DIR}/libs")

set(MAIN_DEPENDENCIES "Boost::process" "Boost::program_options" "boost_dll_iface")

# ========================================== #
# Build Language Support (plugin mode)       #
# ========================================== #
#
# Languages are no longer statically linked via a generated factory header.
# Each language lives in its own self-contained module under languages/<name>/
# and compiles to a shared plugin (.so) in bin/plugins/, loaded at runtime by
# indextools::LangPluginManager (include/plugin_manager.hpp). The old Python
# codegen (generate_lang_factory.py + language_config.json) is retired; a plugin
# self-declares its extensions in its *_plugin.cpp. See languages/README.md.
#
# languages/ auto-discovers its module subdirectories and records their targets
# in the GLOBAL properties INDEX_LANG_PLUGINS / INDEX_LANG_ANALYZER_LIBS.

add_subdirectory(languages)

# ========================================== #
# Build Main                                 #
# ========================================== #

set(EXECUTABLE_NAME "indextools")

file(GLOB SOURCES "src/*.cpp")

add_executable(${EXECUTABLE_NAME} ${SOURCES} main.cpp)
target_compile_options(${EXECUTABLE_NAME} PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_compile_definitions(${EXECUTABLE_NAME} PRIVATE
    $<$<CONFIG:Debug>:DEBUG_BUILD>
    $<$<CONFIG:Release>:NDEBUG>
)
# The host loads plugins at runtime via Boost::dll; it does NOT link the
# language analyzers directly anymore.
target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${MAIN_DEPENDENCIES})
# Ensure every discovered language plugin is built whenever the host is, without
# naming any language here (the list is populated by languages/ auto-discovery).
get_property(_lang_plugins GLOBAL PROPERTY INDEX_LANG_PLUGINS)
if(_lang_plugins)
    add_dependencies(${EXECUTABLE_NAME} ${_lang_plugins})
endif()
if(LTO_SUPPORT)
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
    )
endif()

# ========================================== #
# Unit tests                                 #
# ========================================== #

option(BUILD_TESTS "Build unit tests" ON)
# When ON, test_cache_system is built with -fsanitize=thread so its
# multi-threaded launch_search cases are checked for data races in the cache
# machinery. Off by default: TSan instrumentation slows the build and the
# runtime is only meaningful for the threaded tests, so it is opt-in (configure
# a separate build dir with -DENABLE_TSAN=ON to run it). See test/CMakeLists.txt
# for what this does and does not catch (notably: it will not flag a revert of
# result_channel to a plain channel).
option(ENABLE_TSAN "Instrument test_cache_system with ThreadSanitizer" OFF)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
