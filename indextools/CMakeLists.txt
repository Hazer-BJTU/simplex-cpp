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
#   - the `indextools_lib` static library (consumed by this module's tests
#     and by sibling modules such as the future model adapter / agentloop)
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
# SIMPLEX_VERSIONING_INCLUDE_DIR (set by the sibling versioning/ module) brings
# in the build-generated versioning/version.hpp — used by lang_plugin.hpp.
include_directories(
    "${INDEXTOOLS_INCLUDE_DIR}"
    "${SIMPLEX_THIRDPARTY_DIR}/include"
    "${SIMPLEX_VERSIONING_INCLUDE_DIR}"
)
link_directories("${SIMPLEX_THIRDPARTY_DIR}/libs")

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
# Build indextools_lib (static library)      #
# ========================================== #
#
# The module's reusable logic compiles to a static library so it can be linked
# by this module's unit tests and by sibling modules (model adapter, agentloop).
# PUBLIC include dirs and link deps propagate to every consumer; a downstream
# target only needs `target_link_libraries(... PRIVATE indextools_lib)`.
#
# Note: Boost.program_options used to be listed among the host's dependencies
# but was never referenced by any source/header — it is omitted here. Add it
# back (PUBLIC) should a future CLI source pull it in.

set(LIB_NAME "indextools_lib")

file(GLOB LIB_SOURCES "src/*.cpp")

add_library(${LIB_NAME} STATIC ${LIB_SOURCES})
target_compile_options(${LIB_NAME} PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_compile_definitions(${LIB_NAME} PRIVATE
    $<$<CONFIG:Debug>:DEBUG_BUILD>
    $<$<CONFIG:Release>:NDEBUG>
)
# Headers (the public surface) pull in Boost.Process/Asio and Boost.DLL, so
# those travel PUBLIC: consumers get the include dirs and the link deps without
# redeclaring them. The library does NOT link the language analyzers — plugins
# are loaded at runtime via Boost::dll.
target_include_directories(${LIB_NAME} PUBLIC
    "${INDEXTOOLS_INCLUDE_DIR}"
    "${SIMPLEX_THIRDPARTY_DIR}/include"
    "${SIMPLEX_VERSIONING_INCLUDE_DIR}"
)
target_link_libraries(${LIB_NAME} PUBLIC
    Boost::process
    boost_dll_iface
)
# Ensure every discovered language plugin is built whenever the lib's tests are,
# without naming any language here (the list is populated by languages/
# auto-discovery). Tests that load plugins at runtime need the .so on disk.
get_property(_lang_plugins GLOBAL PROPERTY INDEX_LANG_PLUGINS)
if(_lang_plugins)
    add_dependencies(${LIB_NAME} ${_lang_plugins})
endif()
if(LTO_SUPPORT)
    set_target_properties(${LIB_NAME} PROPERTIES
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

# The cache machinery exercised by test_cache_system lives in indextools_lib,
# so the lib's sources must be compiled with -fsanitize=thread for TSan to see
# the races. The sanitizer runtime is linked at the test executable (see
# test/CMakeLists.txt); here we only add the compile instrumentation to the lib.
if(ENABLE_TSAN)
    target_compile_options(${LIB_NAME} PRIVATE
        -fsanitize=thread -g -O1 -fno-omit-frame-pointer)
endif()

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
