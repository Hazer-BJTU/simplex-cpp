# =============================================================================
# SimplexRelease.cmake — install rules and the $ORIGIN RPATH that makes a
# staged release actually load its own bundled runtime.
#
# ## Why RPATH is the whole point of this file
#
# Copying libstdc++.so.6 and the Boost libraries next to a binary does NOTHING
# on Linux. The dynamic loader does not look beside the executable; it searches
# DT_RPATH/DT_RUNPATH, then LD_LIBRARY_PATH, then the ldconfig cache. With none
# of those pointing at the release directory, a staged binary silently resolves
# libstdc++ from the HOST — which defeats the reason this project bootstraps its
# own GCC: the whole point is shipping the GCC 14 libstdc++, not borrowing
# whatever the target distro happens to have.
#
# That failure is invisible in the build image, where /usr/local/lib64 is
# already in the ldconfig cache. It only appears on a clean target. So the
# install tree gets $ORIGIN-relative RUNPATHs, and CI runs the staged tree in a
# stock ubuntu:22.04 (see .github/workflows/ci.yml) whose libstdc++ is OLDER
# than ours — if RPATH is wrong there, the binary fails to load outright rather
# than passing by accident.
#
# ## Layout
#
#   <prefix>/bin/                 executables
#   <prefix>/bin/plugins/llm/     dlopen'd provider modules
#   <prefix>/lib/                 project shared libraries + bundled runtime
#
# Plugins keep their build-tree-relative position under bin/ because the host
# locates them relative to its own executable (<exe_dir>/plugins — see the
# build-output comment in the top-level CMakeLists). Same layout in both trees
# means no path configuration differs between "tested" and "shipped".
#
# ## What gets installed, and why it is derived
#
# The release set is DERIVED by walking the buildsystem and taking every
# non-INTERFACE target outside a test directory — not enumerated by name. An
# explicit list rots: every new module silently ships nothing until someone
# remembers to add a line, and the failure mode is a release missing a library
# rather than a build error. The `/test` exclusion is what keeps test-only
# artifacts (toyextension, the eventbus DSO fixtures) out of a release.
# =============================================================================

# A release bundle is self-contained, not an FHS installation, so the library
# directory is pinned to `lib` BEFORE GNUInstallDirs gets to decide. Left to
# itself it answers lib64 on RHEL-family distros and lib on Debian — meaning
# the tarball layout, the documented release structure and every $ORIGIN RPATH
# would differ depending on which distro happened to build it. Nothing about a
# bundle whose libraries are found $ORIGIN-relative benefits from that split.
set(CMAKE_INSTALL_LIBDIR "lib" CACHE PATH "Release-relative library directory")
include(GNUInstallDirs)

# ---- Build-tree RPATH stays CMake's default ---------------------------------
# BUILD_WITH_INSTALL_RPATH is deliberately left OFF: the build tree keeps the
# absolute-path RPATH CMake generates, so running tests straight out of build/
# works exactly as it does today. The $ORIGIN form below applies only to the
# installed copies, which CMake relinks on install.
set(CMAKE_SKIP_INSTALL_RPATH OFF)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)

# ---- Walk the buildsystem ---------------------------------------------------
# Recursive because targets live in nested module directories (utils/asio,
# llm/providers/deepseek, ...), and BUILDSYSTEM_TARGETS is per-directory only.
function(_simplex_collect_targets dir out_targets)
    set(_found "")
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        # Test directories are skipped WHOLESALE rather than filtered by target
        # name. Name-based filtering needs every fixture to be recognisably
        # named, which is a convention nobody can enforce; directory position
        # is structural.
        if(_sub MATCHES "/test(/|$)")
            continue()
        endif()
        _simplex_collect_targets("${_sub}" _sub_found)
        list(APPEND _found ${_sub_found})
    endforeach()
    get_property(_here DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    list(APPEND _found ${_here})
    set(${out_targets} "${_found}" PARENT_SCOPE)
endfunction()

_simplex_collect_targets("${CMAKE_SOURCE_DIR}" _all_targets)

# ---- Sort into the three install flavours -----------------------------------
set(_rel_runtime "")   # executables
set(_rel_library "")   # SHARED libraries
set(_rel_module "")    # MODULE libraries (dlopen'd plugins)

foreach(_tgt IN LISTS _all_targets)
    get_target_property(_type ${_tgt} TYPE)
    if(_type STREQUAL "EXECUTABLE")
        list(APPEND _rel_runtime ${_tgt})
    elseif(_type STREQUAL "SHARED_LIBRARY")
        list(APPEND _rel_library ${_tgt})
    elseif(_type STREQUAL "MODULE_LIBRARY")
        list(APPEND _rel_module ${_tgt})
    endif()
    # INTERFACE_LIBRARY and STATIC_LIBRARY fall through on purpose: interface
    # targets have nothing to install, and the tree's static libraries are
    # vendored third-party archives that link INTO the shared libraries rather
    # than shipping beside them.
endforeach()

# ---- Executables and shared libraries ---------------------------------------
# $ORIGIN/../lib gets an executable in bin/ to the shared libraries in lib/;
# $ORIGIN gets a library in lib/ to its siblings there, which is how a bundled
# libboost_*.so finds libstdc++.so.6 in the same directory.
if(_rel_runtime)
    set_target_properties(${_rel_runtime} PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    install(TARGETS ${_rel_runtime}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(_rel_library)
    set_target_properties(${_rel_library} PROPERTIES INSTALL_RPATH "$ORIGIN")
    install(TARGETS ${_rel_library}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# ---- Plugins ----------------------------------------------------------------
# Each plugin's install destination is DERIVED from the LIBRARY_OUTPUT_DIRECTORY
# it already declares, made relative to the build tree's bin/. A plugin that
# builds to bin/plugins/llm installs to <prefix>/bin/plugins/llm, so the
# host's <exe_dir>/plugins lookup resolves identically in both trees — and a
# module that later moves its output directory carries its install location
# with it instead of drifting from a second hardcoded copy here.
foreach(_tgt IN LISTS _rel_module)
    get_target_property(_outdir ${_tgt} LIBRARY_OUTPUT_DIRECTORY)
    if(NOT _outdir)
        message(FATAL_ERROR
            "SimplexRelease: MODULE target ${_tgt} has no LIBRARY_OUTPUT_DIRECTORY; "
            "its release location cannot be derived")
    endif()
    file(RELATIVE_PATH _rel "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" "${_outdir}")

    # Hop back up to <prefix> from the plugin's own nested directory, then down
    # into lib/. Counting the segments keeps this correct at any nesting depth
    # rather than assuming plugins/llm's particular two levels.
    string(REPLACE "/" ";" _segs "${_rel}")
    set(_up "..")            # out of bin/
    foreach(_s IN LISTS _segs)
        set(_up "${_up}/..")
    endforeach()

    set_target_properties(${_tgt} PROPERTIES
        INSTALL_RPATH "$ORIGIN/${_up}/${CMAKE_INSTALL_LIBDIR}")
    install(TARGETS ${_tgt}
        LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}/${_rel}")
endforeach()

message(STATUS "Release install set: ${_rel_runtime} | ${_rel_library} | ${_rel_module}")
