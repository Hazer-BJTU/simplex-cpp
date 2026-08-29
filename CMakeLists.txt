# =============================================================================
# simplex-cpp — top-level build
#
# This is a multi-module C++ project. Each module lives in its own
# subdirectory (utils, extension_framework, endpoint, dataclass, llm, …) and
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
# layer) and filesystem (the Boost.DLL support target below). The legacy
# language-index domain (indextools) additionally used process and
# program_options; it is no longer part of this build (pending extraction into
# its own module) and takes those requirements with it.
#
# Linked SHARED deliberately: the whole tree now standardizes on shared
# libraries (the plugin-boundary strategy in docs/abi-context.md — one copy of
# every compiled runtime per process), and the Boost runtime objects are
# distributed dependencies that a later packaging step bundles alongside the
# project's own .so files. Static Boost would silently give every executable
# its own private copy of the compiled components again.
set(Boost_USE_STATIC_LIBS OFF)

find_package(Boost REQUIRED COMPONENTS unit_test_framework filesystem)

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
# extension_framework after utils' logging + the top-level interface targets:
# it is a header-only module (extension_framework_iface) that links
# boost_dll_iface, logging_lib, and simplex_thirdparty_iface.
add_subdirectory(extension_framework)
# endpoint after utils: endpoint_lib links logging_lib (the public
# https_connection.hpp uses logging::Logger) and the shared Boost/OpenSSL.
add_subdirectory(endpoint)
# dataclass after the shared third-party interface: a header-only module
# (dataclass_iface) defining the model_io data contract. Every record is a
# plain aggregate serialised through nlohmann ADL to_json/from_json free
# functions, so the module depends only on simplex_thirdparty_iface
# (for <nlohmann/json.hpp>).
add_subdirectory(dataclass)
# process after dataclass: the child-process management module. Its typed
# data contract (process_io::LaunchSpec / ExecutionStatus / ExecutionResult)
# lives in dataclass; the exception contract lands first, the manager
# framework (Boost.Process v2 + asio) lands next and will add the process
# component to the top-level find_package and Boost::process + asio_iface
# to this target.
add_subdirectory(process)
# llm after dataclass + extension_framework + versioning: the model-plugin
# contract (LLMModel / LLMModelExtensionContext / LLMDispatcher), canonical
# Responses adapter, and provider plugins build on the generic extension
# framework, endpoint transport and model_io data contract. Runtime providers
# are emitted under <exe>/plugins/llm.
add_subdirectory(llm)
