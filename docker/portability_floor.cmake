# =============================================================================
# portability_floor.cmake — assert the build tree's artifacts can actually RUN
# on the oldest platform the release targets (run as a ctest via `cmake -P`;
# zero dependencies beyond readelf, from binutils).
#
# ## What this catches, and why a normal build cannot
#
# glibc is backward- but not forward-compatible, and it versions its symbols:
# a reference to `pow@GLIBC_2.29` resolves fine on a newer host and fails at
# LOAD time on an older one. Nothing in a successful compile, link, or test run
# reveals this — the build machine always has a new enough glibc, by
# construction. The floor is a property of the BUILD IMAGE that only becomes
# visible when someone runs the release on an older distro. This script makes
# it visible in CI instead.
#
# Three ways the floor silently rises, all caught here:
#   1. The base image moves (the reason this script exists: descending from
#      gcc:14.3.0 = Debian 13 put the floor at glibc 2.41, unusable for
#      release).
#   2. A new call reaches a symbol that got a fresh version node — e.g. any
#      of the *_time64 family, or `exp`/`pow`'s GLIBC_2.29 revisions.
#   3. A dependency starts pulling one in, with no source change at all.
#
# ## What it does NOT check
#
# Only the glibc/GLIBCXX/CXXABI version floors and the DT_NEEDED set. It does
# not prove the release RUNS — a symbol present at the right version can still
# behave differently, and kernel/driver requirements are out of scope. Actually
# executing the artifacts on the target distro is a separate, complementary
# test; this one is the cheap gate that runs on every commit.
#
# ## Inputs (via -D)
#   BUILD_DIR      required  build tree to scan (recursively)
#   GLIBC_FLOOR    required  max allowed glibc version, e.g. 2.34
#   GLIBCXX_FLOOR  optional  max allowed GLIBCXX_3.4.x minor, e.g. 33
#   STRICT_NEEDED  optional  ON => DT_NEEDED must be on the allowlist below
# =============================================================================
cmake_minimum_required(VERSION 3.20)

foreach(_req IN ITEMS BUILD_DIR GLIBC_FLOOR)
    if(NOT DEFINED ${_req})
        message(FATAL_ERROR "portability_floor: ${_req} must be defined")
    endif()
endforeach()

find_program(READELF_EXECUTABLE NAMES readelf REQUIRED)

# Shared objects a redistributable artifact may depend on, beyond the
# project's own libraries (those are derived from the build tree below).
# Everything here is either provided by any glibc host (the C runtime),
# shipped alongside the release (libstdc++/libgcc_s/libboost_*, see the build
# image's /opt/simplex-runtime), or a documented external requirement
# (OpenSSL 3). Anything else means the release grew a dependency nobody
# planned to ship — the case this list exists to surface.
set(_allowed_needed
    "libssl.so.3" "libcrypto.so.3"
    "libstdc++.so.6" "libgcc_s.so.1"
    "libm.so.6" "libc.so.6" "libdl.so.2" "librt.so.1" "libpthread.so.0"
    "ld-linux-x86-64.so.2")

# ELF artifacts only: executables and shared objects. Static archives carry no
# dynamic symbol table, so their symbol versions materialise in whatever links
# them and are covered there.
file(GLOB_RECURSE _candidates "${BUILD_DIR}/*")
set(_artifacts "")
foreach(_f ${_candidates})
    if(IS_DIRECTORY "${_f}")
        continue()
    endif()
    # CMake's own scratch (CMakeFiles/, try_compile leftovers) is not release
    # output and would add noise plus a lot of readelf calls.
    if(_f MATCHES "/CMakeFiles/")
        continue()
    endif()
    # Static archives and build metadata: skipped by extension before the
    # (cheap, but not free) magic-byte read.
    if(_f MATCHES "\\.(a|o|cmake|txt|json|log|ninja|md|yaml|yml|hpp|cpp|h)$")
        continue()
    endif()
    # An ELF starts with 0x7F 'E' 'L' 'F'; read 4 bytes rather than shelling
    # out to file(1), which is not guaranteed installed.
    file(READ "${_f}" _magic OFFSET 0 LIMIT 4 HEX)
    if(_magic STREQUAL "7f454c46")
        list(APPEND _artifacts "${_f}")
    endif()
endforeach()

if(NOT _artifacts)
    message(FATAL_ERROR
        "portability_floor: no ELF artifacts found under ${BUILD_DIR} — "
        "registering zero checks must not read as green")
endif()

# The project's own shared libraries, DERIVED from what the tree actually
# built rather than listed by name. A DT_NEEDED naming one of these is
# self-evidently fine: it ships in the same release as the artifact needing
# it. Hardcoding the names instead makes the check fail every time a module
# grows a shared library — a maintenance tax that teaches people to widen the
# allowlist reflexively, which is how an allowlist stops meaning anything.
set(_own_libs "")
foreach(_art ${_artifacts})
    get_filename_component(_n "${_art}" NAME)
    if(_n MATCHES "^(lib[^/]*\\.so)($|\\.)")
        list(APPEND _own_libs "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _own_libs)

set(_failures "")
set(_max_glibc "0.0")
set(_max_glibcxx 0)

# Compare dotted versions numerically: string compare would rank 2.9 above
# 2.34, which is exactly the mistake this file is here to prevent.
function(_version_gt lhs rhs out)
    if("${lhs}" VERSION_GREATER "${rhs}")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

foreach(_art ${_artifacts})
    execute_process(
        COMMAND ${READELF_EXECUTABLE} --dyn-syms --dynamic --wide "${_art}"
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _res)
    if(NOT _res EQUAL 0)
        list(APPEND _failures "readelf failed on ${_art}")
        continue()
    endif()
    get_filename_component(_name "${_art}" NAME)

    # --- glibc symbol versions ------------------------------------------------
    # Undefined symbols carry @GLIBC_x.y for the version node they bind to.
    # ONE regex pass per artifact, capturing symbol AND version together, then
    # filtered in CMake. Matching per-version instead would re-scan the whole
    # readelf output once per version node present — with a low floor that is
    # every node, and the scan goes from seconds to minutes.
    string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_.]*@GLIBC_[0-9]+\\.[0-9]+(\\.[0-9]+)?"
        _refs "${_out}")
    list(REMOVE_DUPLICATES _refs)
    set(_over_syms "")
    foreach(_ref ${_refs})
        string(REGEX REPLACE "^.*@GLIBC_" "" _ver "${_ref}")
        _version_gt("${_ver}" "${_max_glibc}" _newer)
        if(_newer)
            set(_max_glibc "${_ver}")
        endif()
        _version_gt("${_ver}" "${GLIBC_FLOOR}" _over)
        if(_over)
            # Name the symbols, not just the version: the fix depends on which
            # call pulled the node in (a header-driven redirect like
            # __isoc23_strtol needs an older base image, not a source change).
            list(APPEND _over_syms "${_ref}")
        endif()
    endforeach()
    if(_over_syms)
        list(LENGTH _over_syms _n_over)
        list(SORT _over_syms)
        if(_n_over GREATER 6)
            list(SUBLIST _over_syms 0 6 _shown)
            list(APPEND _shown "... (+${_n_over} total)")
        else()
            set(_shown "${_over_syms}")
        endif()
        list(JOIN _shown ", " _sym_text)
        list(APPEND _failures
            "${_name}: requires glibc newer than floor ${GLIBC_FLOOR}: ${_sym_text}")
    endif()

    # --- libstdc++ symbol versions -------------------------------------------
    # Informational unless GLIBCXX_FLOOR is given: libstdc++ is shipped with
    # the release, so a high GLIBCXX is fine as long as the shipped .so
    # provides it. The floor is worth pinning when the release is expected to
    # run against a HOST libstdc++ instead.
    string(REGEX MATCHALL "@GLIBCXX_3\\.4\\.[0-9]+" _cxx_refs "${_out}")
    list(REMOVE_DUPLICATES _cxx_refs)
    foreach(_ref ${_cxx_refs})
        string(REGEX REPLACE "^@GLIBCXX_3\\.4\\." "" _minor "${_ref}")
        if(_minor GREATER _max_glibcxx)
            set(_max_glibcxx ${_minor})
        endif()
        # Nested rather than a single AND: when GLIBCXX_FLOOR is unset,
        # ${GLIBCXX_FLOOR} expands to nothing and leaves GREATER without a
        # right operand, which is a hard CMake parse error rather than false.
        if(DEFINED GLIBCXX_FLOOR)
            if(_minor GREATER ${GLIBCXX_FLOOR})
                list(APPEND _failures
                    "${_name}: requires GLIBCXX_3.4.${_minor} > floor 3.4.${GLIBCXX_FLOOR}")
            endif()
        endif()
    endforeach()

    # --- DT_NEEDED allowlist -------------------------------------------------
    if(STRICT_NEEDED)
        string(REGEX MATCHALL "Shared library: \\[[^]]*\\]" _needed "${_out}")
        foreach(_line ${_needed})
            string(REGEX REPLACE ".*\\[([^]]*)\\]" "\\1" _lib "${_line}")
            list(FIND _allowed_needed "${_lib}" _idx)
            list(FIND _own_libs "${_lib}" _own_idx)
            # Boost is matched by prefix rather than enumerated: it is shipped
            # in /opt/simplex-runtime as a family, and which components the
            # tree links is a build-configuration detail this check has no
            # stake in.
            if(_idx EQUAL -1 AND _own_idx EQUAL -1
               AND NOT _lib MATCHES "^libboost_")
                list(APPEND _failures "${_name}: unexpected DT_NEEDED ${_lib}")
            endif()
        endforeach()
    endif()
endforeach()

list(LENGTH _artifacts _n)
if(_failures)
    list(REMOVE_DUPLICATES _failures)
    list(JOIN _failures "\n  - " _text)
    message(FATAL_ERROR
        "portability floor FAILED (${_n} artifacts scanned, floor glibc ${GLIBC_FLOOR}):\n  - ${_text}")
endif()
message(STATUS
    "portability floor: ${_n} artifacts scanned, highest glibc requirement "
    "GLIBC_${_max_glibc} (floor ${GLIBC_FLOOR}), highest GLIBCXX_3.4.${_max_glibcxx}")
