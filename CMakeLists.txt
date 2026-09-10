# =============================================================================
# simplex-cpp — top-level build
#
# This is a multi-module C++ project. Each module lives in its own
# subdirectory (utils, extensions, endpoint, dataclass, llm, …) and
# is wired in with `add_subdirectory()` below.
#
# What belongs HERE (the top level) and not inside a module:
#   - the toolchain & language standard           (shared by every module)
#   - the LTO probe                                (shared)
#   - shared third-party deps that more than one module uses (Boost)
#   - the bundled `third_party/` tree location     (single source of truth)
#   - the build-output directories                 (keeps the source tree clean)
#
# What stays inside a module:
#   - the module's own targets, sources, and unit tests
#   - module-private third-party deps (e.g. python's tree-sitter, declared in
#     the module's own CMakeLists so the dependency travels with the module)
#
# Per-module dependencies are documented in `dependency.md`.
# =============================================================================

cmake_minimum_required(VERSION 3.20)

# ---- Toolchain --------------------------------------------------------------
# The project targets g++-14. Pin it here so a bare `cmake ..` picks the right
# compiler without requiring -DCMAKE_CXX_COMPILER on the command line. This
# must be set before project() to take effect.
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER g++-14)
endif()

project(simplex_cpp VERSION 0.0.1 LANGUAGES CXX)

# ---- LTO probe (shared) -----------------------------------------------------
include(CheckIPOSupported)
check_ipo_supported(RESULT LTO_SUPPORT OUTPUT LTO_CHECK_OUTPUT)
message(STATUS "Check LTO support: ${LTO_SUPPORT}")

# ---- Language standard (shared) --------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Position-independent code (shared) ------------------------------------
# -fPIC is the rule here, not a per-target opt-in: libraries in this tree end
# up loaded into processes that also dlopen plugin modules
# (docs/abi-context.md). SHARED/MODULE targets get PIC from the compiler
# defaults anyway; this makes STATIC libraries — and executables, as -fPIE —
# default to it too. No target sets POSITION_INDEPENDENT_CODE individually.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Build output directories ----------------------------------------------
# All module artifacts land under the build tree (bin/ + lib/), not in the
# source tree. The `indextools` host locates its plugins relative to its own
# executable directory (<exe_dir>/plugins), so plugins emitted into bin/plugins
# are found at runtime with no path configuration.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---- Bundled third-party tree ----------------------------------------------
# Single source of truth for the vendored headers/static libs shared across
# modules. Exposed as a variable (propagates to every add_subdirectory child)
# plus an INTERFACE target that brings the include dir + lib search path.
# Overridable on the command line: container builds point it at the copy the
# build-base image carries under /opt/simplex-thirdparty (see
# Dockerfile.build-context) instead of a populated in-tree third_party/.
if(NOT SIMPLEX_THIRDPARTY_DIR)
    set(SIMPLEX_THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")
endif()

add_library(simplex_thirdparty_iface INTERFACE)
target_include_directories(simplex_thirdparty_iface INTERFACE
    "${SIMPLEX_THIRDPARTY_DIR}/include")
target_link_directories(simplex_thirdparty_iface INTERFACE
    "${SIMPLEX_THIRDPARTY_DIR}/libs")

# ---- Shared dependency: OpenSSL ---------------------------------------------
# Used by modules that need TLS/SSL connectivity (HTTPS clients, secure IPC,
# cryptographic hashing). Found here so every module links the same system
# OpenSSL; CMake's FindOpenSSL provides the imported targets OpenSSL::SSL and
# OpenSSL::Crypto.
find_package(OpenSSL REQUIRED)

add_library(openssl_iface INTERFACE)
target_link_libraries(openssl_iface INTERFACE OpenSSL::SSL OpenSSL::Crypto)

# ---- Shared dependency: Boost ----------------------------------------------
# Compiled components used by the core tree: unit_test_framework (the test
# layer), filesystem (the Boost.DLL support target below) and process (the
# boost_process_iface target below — Boost.Process v2 grew a compiled piece,
# libboost_process, so it needs the component, not just headers). Of the
# legacy language-index domain's extras only program_options is still gone;
# it stays with the extraction into a standalone module.
#
# Linked SHARED deliberately: the whole tree now standardizes on shared
# libraries (the plugin-boundary strategy in docs/abi-context.md — one copy of
# every compiled runtime per process), and the Boost runtime objects are
# distributed dependencies that a later packaging step bundles alongside the
# project's own .so files. Static Boost would silently give every executable
# its own private copy of the compiled components again.
set(Boost_USE_STATIC_LIBS OFF)

find_package(Boost REQUIRED COMPONENTS unit_test_framework filesystem process)

# Boost.DLL is header-only (no compiled component / CMake config), but it
# depends on Boost.Filesystem, Boost.System and, on Linux, libdl. Bundle those
# into one interface target the host and plugins can link uniformly.
add_library(boost_dll_iface INTERFACE)
target_link_libraries(boost_dll_iface INTERFACE
    Boost::headers Boost::filesystem ${CMAKE_DL_LIBS})
# Make Boost.DLL speak std::filesystem::path / std::error_code so it composes
# with the std::filesystem used throughout the codebase. Must be defined
# consistently in every TU that includes a Boost.DLL header (host + plugins).
target_compile_definitions(boost_dll_iface INTERFACE BOOST_DLL_USE_STD_FS)

# Boost.Process v2 is the process/ module's manager foundation. Modern Boost
# compiles a piece of it (libboost_process), so it joins Boost.DLL as a
# top-level interface target instead of per-module requirements: consumers
# link boost_process_iface, never raw Boost::process. Asio stays out of this
# target on purpose — the manager's coroutine layer takes asio_iface
# (utils/asio) so every consumer binds the one shared Asio runtime.
add_library(boost_process_iface INTERFACE)
target_link_libraries(boost_process_iface INTERFACE
    Boost::process Boost::headers Boost::filesystem)

# ---- Modules ----------------------------------------------------------------
# enable_testing() at the top level so `ctest` run from the build root discovers
# tests registered in any module's test/ subdirectory. (Calling it only inside a
# module would scope CTest to that module's build dir and hide tests from a
# top-level `ctest` run.)
enable_testing()

# Project-wide test switch. Declared here (not inside a module) so every
# module's test/ subdirectory sees the same option.
option(BUILD_TESTS "Build unit tests" ON)

# Module add order matters: each module may consume targets or GLOBAL properties
# populated by an earlier one.
#   - versioning first: its `simplex_versioning` INTERFACE target feeds every
#     plugin contract in the tree (the LLM plugin ABI version today).
#   - utils early: the small shared utilities package. Its logging/ member
#     exports `logging_lib` (endpoint's public headers use logging::Logger);
#     its yamlconfig/ member is the header-only YAML->JSON config bridge
#     (yamlconfig_iface) feeding JSON-shaped config consumers such as the llm
#     model plugins' config JSON.
#
# The legacy language-index domain — languages/ (per-language analyzer
# plugins) and indextools/ (their host library) — is NOT wired in anymore:
# nothing in the core tree links or loads it, and it is pending extraction
# into a standalone module/plugin. The directories stay in the repository
# untouched; when extracted, they carry their own dependencies (notably Boost
# process and the vendored tree-sitter libs).
add_subdirectory(versioning)
add_subdirectory(utils)
# extensions after utils' logging + the top-level interface targets:
# it is a header-only module (extensions_iface) that links
# boost_dll_iface, logging_lib, and simplex_thirdparty_iface.
add_subdirectory(extensions)
# endpoint after utils: endpoint_lib links logging_lib (the public
# https_connection.hpp uses logging::Logger) and the shared Boost/OpenSSL.
add_subdirectory(endpoint)
# intercom after endpoint: internal service-to-service WebSocket transport,
# which reuses endpoint's connection primitives (connect_flavour, the stream
# flavours, ResolvedEndpoint, retry_policy backoff).
add_subdirectory(intercom)
# dataclass after the shared third-party interface: a header-only module
# (dataclass_iface) defining the model_io data contract. Every record is a
# plain aggregate serialised through nlohmann ADL to_json/from_json free
# functions, so the module depends only on simplex_thirdparty_iface
# (for <nlohmann/json.hpp>).
add_subdirectory(dataclass)
# process after dataclass: the child-process management module. Its typed
# data contract (process::LaunchSpec / ExecutionStatus / ExecutionResult)
# lives in dataclass; the exception contract lands first. The manager
# framework (Boost.Process v2 + asio) lands next and links
# boost_process_iface (top level) + asio_iface (utils/asio) — both are
# already in place for it.
add_subdirectory(process)
# llm after dataclass + extensions + versioning: the model-plugin
# contract (LLMModel / LLMModelExtensionContext / LLMDispatcher), canonical
# Responses adapter, and provider plugins build on the generic extension
# framework, endpoint transport and model_io data contract. Runtime providers
# are emitted under <exe>/plugins/llm.
add_subdirectory(llm)

# ---- Release install rules --------------------------------------------------
# Included AFTER every add_subdirectory() above: it derives the release set by
# walking the buildsystem, so the targets must already exist. Defines the
# install() rules and the $ORIGIN RPATHs that let a staged release load its own
# bundled libstdc++/Boost instead of the host's.
#
# SIMPLEX_BUNDLE_RUNTIME points at a directory of runtime libraries to install
# alongside the project's own (the build image collects them into
# /opt/simplex-runtime). Empty by default: a dev build installs nothing extra
# and picks the runtime up from the system, which is what a dev wants.
set(SIMPLEX_BUNDLE_RUNTIME "" CACHE PATH
    "Directory of runtime libraries (libstdc++, libgcc_s, libboost_*) to install into lib/")

include(cmake/SimplexRelease.cmake)

if(SIMPLEX_BUNDLE_RUNTIME)
    if(NOT IS_DIRECTORY "${SIMPLEX_BUNDLE_RUNTIME}")
        message(FATAL_ERROR
            "SIMPLEX_BUNDLE_RUNTIME=${SIMPLEX_BUNDLE_RUNTIME} is not a directory")
    endif()
    # install(DIRECTORY) copies symlinks AS symlinks, which is what we want
    # here: the source directory holds both libstdc++.so.6 and its target
    # libstdc++.so.6.0.33 (the image collects them with `cp -P`), so the link
    # and the real file arrive together. Copying only one would leave the
    # loader with a dangling symlink — the same failure as shipping nothing.
    # The *-gdb.py exclusion is not cosmetic: libstdc++ installs a pretty-printer
    # script next to the library, matching *.so* by name only. Shipping it puts a
    # Python file in a release lib/ directory that gdb would auto-load from a
    # path the release does not control.
    install(DIRECTORY "${SIMPLEX_BUNDLE_RUNTIME}/"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        FILES_MATCHING PATTERN "*.so*" PATTERN "*-gdb.py" EXCLUDE)
    message(STATUS "Bundling runtime libraries from ${SIMPLEX_BUNDLE_RUNTIME}")
endif()

# ---- Portability floor (opt-in) ---------------------------------------------
# Assert that nothing in the build tree references a glibc symbol version newer
# than the oldest platform a release targets. This is a property of the BUILD
# IMAGE, not of the source: a successful compile+link+ctest run never reveals
# it, because the build machine's glibc is new enough by construction. See
# docker/portability_floor.cmake for what it does and does not prove.
#
# Off by default and deliberately so: the ordinary dev build (local WSL, or the
# Debian-13-based docker/Dockerfile.build-base) sits far above any release
# floor, and failing those builds over it would be noise. It is enabled in the
# portable build image only — docker/Dockerfile.build-portable, whose whole
# purpose is to keep this number low.
#
#   -DSIMPLEX_GLIBC_FLOOR=2.34    max allowed glibc version
#   -DSIMPLEX_STRICT_NEEDED=ON    additionally allowlist DT_NEEDED
#   -DSIMPLEX_OPENSSL_FLOOR=3.0.0 max allowed OPENSSL_3.x.y symbol node
#   -DSIMPLEX_STAGING_DIR=...     installed release tree; required by STRICT_NEEDED
set(SIMPLEX_GLIBC_FLOOR "" CACHE STRING
    "If set (e.g. 2.34), register a ctest asserting no artifact requires a newer glibc")
set(SIMPLEX_OPENSSL_FLOOR "" CACHE STRING
    "With SIMPLEX_GLIBC_FLOOR, max allowed OpenSSL symbol version node (e.g. 3.0.0)")
set(SIMPLEX_STAGING_DIR "" CACHE PATH
    "Installed release tree the floor test treats as the set of shipped libraries")
option(SIMPLEX_STRICT_NEEDED
    "With SIMPLEX_GLIBC_FLOOR, also assert every DT_NEEDED is allowlisted or staged" OFF)

if(SIMPLEX_GLIBC_FLOOR)
    # Optional inputs are passed only when set. Forwarding an empty -D would
    # make the script see them as DEFINED-but-blank and compare against
    # nothing, which is how an unset floor turns into a floor of zero.
    set(_floor_args "")
    if(SIMPLEX_OPENSSL_FLOOR)
        list(APPEND _floor_args -DOPENSSL_FLOOR=${SIMPLEX_OPENSSL_FLOOR})
    endif()
    if(SIMPLEX_STAGING_DIR)
        list(APPEND _floor_args -DSTAGING_DIR=${SIMPLEX_STAGING_DIR})
    endif()

    add_test(NAME portability_floor COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${CMAKE_BINARY_DIR}
        -DGLIBC_FLOOR=${SIMPLEX_GLIBC_FLOOR}
        -DSTRICT_NEEDED=${SIMPLEX_STRICT_NEEDED}
        ${_floor_args}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/docker/portability_floor.cmake)
    # The scan reads the built artifacts, so it must not run before they exist.
    # ctest has no build ordering of its own; RUN_SERIAL keeps it from racing
    # other tests that could still be writing outputs (none today, but the
    # scan is cheap and this removes the question).
    set_tests_properties(portability_floor PROPERTIES RUN_SERIAL ON)
    message(STATUS "Portability floor test enabled: glibc <= ${SIMPLEX_GLIBC_FLOOR}"
        " (strict DT_NEEDED: ${SIMPLEX_STRICT_NEEDED})")
endif()
