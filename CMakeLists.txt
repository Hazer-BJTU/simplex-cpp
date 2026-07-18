# =============================================================================
# simplex-cpp — top-level build
#
# This is a multi-module C++ project. Each module lives in its own
# subdirectory (currently `indextools/`; future modules such as the native
# model-request adapter and `agentloop` will live alongside it) and is wired in
# with `add_subdirectory()` below.
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

project(simplex_cpp LANGUAGES CXX)

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
set(SIMPLEX_THIRDPARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")

add_library(simplex_thirdparty_iface INTERFACE)
target_include_directories(simplex_thirdparty_iface INTERFACE
    "${SIMPLEX_THIRDPARTY_DIR}/include")
target_link_directories(simplex_thirdparty_iface INTERFACE
    "${SIMPLEX_THIRDPARTY_DIR}/libs")

# ---- Shared dependency: Boost ----------------------------------------------
# Used by the indextools host (process / program_options / DLL) and by the
# unit-test layer (unit_test_framework). Future modules (model adapter,
# agentloop) are expected to reuse Boost.Asio/Process, so the find lives here.
set(Boost_USE_STATIC_LIBS ON)

find_package(Boost REQUIRED COMPONENTS
    unit_test_framework process program_options filesystem)

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

add_subdirectory(indextools)
