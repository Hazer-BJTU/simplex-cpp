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
#   - the `indextools_iface` INTERFACE target (the plugin contract headers —
#     consumed by the sibling `languages/` module to compile analyzer/plugin
#     targets)
#   - its unit tests (via test/)
#
# The language plugin modules live in the sibling `languages/` directory (added
# at the top level, before this module, so its GLOBAL property registrations are
# visible here). See languages/README.md for the plugin contract.
#
# Header management is interface-target-only: there are no directory-scope
# include_directories()/link_directories() calls and no propagated path
# variables here. Every consumer links `indextools_iface` (which itself links
# `simplex_thirdparty_iface` and `simplex_versioning`) to get the indextools,
# third_party, and versioning include paths. Exported indextools headers live
# under include/indextools/ and are included as "indextools/<name>.hpp".
# =============================================================================

# ========================================== #
# indextools_iface (plugin contract headers) #
# ========================================== #
#
# The sibling `languages/` module compiles each analyzer and plugin against the
# indextools plugin contract (indextools/lang.hpp + indextools/lang_plugin.hpp).
# Those headers are header-only, so a plugin .so needs only the include paths —
# not indextools_lib itself. This INTERFACE target is the single bundle for the
# contract:
#   - indextools/include          (indextools/lang.hpp, lang_plugin.hpp, …)
#   - simplex_thirdparty_iface    (nlohmann/json, glob — pulled in by lang.hpp;
#                                  also carries the third_party lib search dir)
#   - simplex_versioning          (LANG_PLUGIN_ABI_VERSION, pulled in by
#                                  lang_plugin.hpp)
# A language target links this (PRIVATE for plugins, PUBLIC for analyzer libs
# whose own public headers #include indextools/lang.hpp) and gets all three.
add_library(indextools_iface INTERFACE)
target_include_directories(indextools_iface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(indextools_iface INTERFACE
    simplex_thirdparty_iface
    simplex_versioning
)

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
# redeclaring them. The include dirs come from indextools_iface (the single
# source of truth for the header contract) linked PUBLIC here, so indextools_lib
# and the sibling languages/ module see the same paths. `logging_lib` is also
# PUBLIC because the public service_command.hpp header uses logging::Logger.
# The library does NOT link the language analyzers — plugins are loaded at
# runtime via Boost::dll.
target_link_libraries(${LIB_NAME} PUBLIC
    indextools_iface
    logging_lib
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

option(ENABLE_TSAN "Instrument test_cache_system with ThreadSanitizer" OFF)
# When ON, test_cache_system is built with -fsanitize=thread so its
# multi-threaded launch_search cases are checked for data races in the cache
# machinery. Off by default: TSan instrumentation slows the build and the
# runtime is only meaningful for the threaded tests, so it is opt-in (configure
# a separate build dir with -DENABLE_TSAN=ON to run it). See test/CMakeLists.txt
# for what this does and does not catch (notably: it will not flag a revert of
# result_channel to a plain channel).

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
