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
# OpenSSL 3 is checked the same way, and it matters for the same reason: it
# versions its symbols per MINOR release (OPENSSL_3.0.0, OPENSSL_3.2.0, ...).
# The image builds against 3.5.x while the oldest supported target ships 3.0.2,
# so a call to any API added after 3.0 binds a version node no Ubuntu 22.04 can
# satisfy — while DT_NEEDED still reads exactly `libssl.so.3`. A SONAME
# allowlist cannot see that; a version-node floor can.
#
# ## What it does NOT check
#
# Only the glibc/GLIBCXX/CXXABI/OpenSSL version floors and the DT_NEEDED set. It
# does not prove the release RUNS — a symbol present at the right version can
# still behave differently, and kernel/driver requirements are out of scope.
# Actually executing the artifacts on the target distro is a separate,
# complementary test (the `staged-runtime` CI job); this one is the cheap gate
# that runs on every commit.
#
# ## Inputs (via -D)
#   BUILD_DIR      required  build tree to scan (recursively)
#   GLIBC_FLOOR    required  max allowed glibc version, e.g. 2.34
#   STAGING_DIR    optional  installed release tree; REQUIRED by STRICT_NEEDED,
#                            and the only thing that counts as "shipped by us"
#   OPENSSL_FLOOR  optional  max allowed OPENSSL_3.x.y node, e.g. 3.0.0
#   GLIBCXX_FLOOR  optional  max allowed GLIBCXX_3.4.x minor, e.g. 33
#   CXXABI_FLOOR   optional  max allowed CXXABI_1.3.x minor, e.g. 15
#   STRICT_NEEDED  optional  ON => DT_NEEDED must be allowlisted or staged
# =============================================================================
cmake_minimum_required(VERSION 3.20)

foreach(_req IN ITEMS BUILD_DIR GLIBC_FLOOR)
    if(NOT DEFINED ${_req})
        message(FATAL_ERROR "portability_floor: ${_req} must be defined")
    endif()
endforeach()

find_program(READELF_EXECUTABLE NAMES readelf REQUIRED)

# Shared objects a redistributable artifact may depend on WITHOUT the release
# shipping them. Two kinds only:
#   - the C runtime, which any glibc >= the floor provides (libm/libc/libdl/
#     librt/libpthread, all of which glibc 2.34 folds into libc.so.6 while
#     keeping the stub SONAMEs);
#   - documented external requirements (OpenSSL 3, deliberately not bundled —
#     see the OpenSSL floor check below, which is what keeps its ABI in scope).
#
# libstdc++, libgcc_s and the Boost family are NOT here, and their absence is
# the point: the release ships them (see /opt/simplex-runtime), so they must
# come back through _own_libs — the SONAMEs actually present in the staging
# tree. Allowlisting them "because they are bundled anyway" would accept a
# release that forgot to bundle them, i.e. exactly the failure that resolves
# libstdc++ from the host instead of from the release. A library is either in
# the release or the host is expected to provide it; there is no third case.
set(_allowed_needed
    "libssl.so.3" "libcrypto.so.3"
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

# The staged tree is scanned TOO, not instead: it holds the bundled runtime
# (libstdc++, libgcc_s, the Boost family) which is part of the release and
# subject to the same floor, and which appears nowhere in the build tree. A
# bundled library that itself needs a newer glibc than the floor breaks the
# release just as thoroughly as a project binary that does.
if(DEFINED STAGING_DIR)
    # Normalized once, because every use below is a prefix test against artifact
    # paths: a trailing slash (which a hand-written -D can easily carry) would
    # make "${STAGING_DIR}/" a doubled separator that matches nothing.
    get_filename_component(STAGING_DIR "${STAGING_DIR}" ABSOLUTE)
    file(GLOB_RECURSE _stage_candidates "${STAGING_DIR}/*")
    foreach(_f ${_stage_candidates})
        if(IS_DIRECTORY "${_f}" OR IS_SYMLINK "${_f}")
            continue()
        endif()
        file(READ "${_f}" _magic OFFSET 0 LIMIT 4 HEX)
        if(_magic STREQUAL "7f454c46")
            list(APPEND _artifacts "${_f}")
        endif()
    endforeach()
endif()

# ---- What counts as "shipped by us" -----------------------------------------
# Derived, not enumerated — a hardcoded list makes the check fail every time a
# module grows a shared library, which teaches people to widen the allowlist
# reflexively, and that is how an allowlist stops meaning anything.
#
# But derived from the STAGING tree, not the build tree. Deriving from
# BUILD_DIR accepts any .so that happens to appear under build/ — a test
# fixture, a FetchContent by-product, a vendored dependency someone builds
# shared — as though the release shipped it. It does not: nothing under build/
# reaches a user. Staging is the only place where "this library exists" and
# "this library ships" are the same statement, which is the invariant
# STRICT_NEEDED is supposed to assert.
#
# Identity comes from DT_SONAME rather than the filename, because DT_NEEDED
# records the SONAME. The two coincide today; they stop coinciding the moment
# any target sets SOVERSION, at which point a filename-derived list would
# reject the project's own libraries.
set(_own_libs "")
if(DEFINED STAGING_DIR)
    if(NOT IS_DIRECTORY "${STAGING_DIR}")
        message(FATAL_ERROR "portability_floor: STAGING_DIR=${STAGING_DIR} is not a directory")
    endif()
    file(GLOB_RECURSE _staged "${STAGING_DIR}/*")
    foreach(_f ${_staged})
        if(IS_DIRECTORY "${_f}" OR IS_SYMLINK "${_f}")
            continue()
        endif()
        file(READ "${_f}" _magic OFFSET 0 LIMIT 4 HEX)
        if(NOT _magic STREQUAL "7f454c46")
            continue()
        endif()
        execute_process(COMMAND ${READELF_EXECUTABLE} -d --wide "${_f}"
            OUTPUT_VARIABLE _dyn ERROR_QUIET RESULT_VARIABLE _dres)
        if(_dres EQUAL 0 AND _dyn MATCHES "SONAME[^[]*\\[([^]]+)\\]")
            list(APPEND _own_libs "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _own_libs)
    list(LENGTH _own_libs _n_own)
    if(_n_own EQUAL 0)
        message(FATAL_ERROR
            "portability_floor: STAGING_DIR=${STAGING_DIR} yielded no SONAMEs — "
            "an empty ship-set would silently accept nothing, or everything")
    endif()
    message(STATUS "portability floor: ${_n_own} staged SONAMEs treated as shipped")
elseif(STRICT_NEEDED)
    # Refuse rather than fall back to the build tree. A weaker check running
    # under the name STRICT_NEEDED is worse than no check: it reports green for
    # a property it never tested.
    message(FATAL_ERROR
        "portability_floor: STRICT_NEEDED requires STAGING_DIR — without it there "
        "is no way to tell a shipped library from a build by-product")
endif()

# ---- Version nodes a distribution BACKPORTED ---------------------------------
# A platform can provide a symbol under a version node newer than its glibc
# version, because it backported the symbol and kept the upstream node name. A
# reference to one is not a floor raise — the platform the release targets does
# have the symbol — and failing on it would report a limitation that does not
# exist. Exactly one qualifies here:
#
#   _dl_find_object@GLIBC_2.35
#     Added upstream in glibc 2.35 and backported into the glibc 2.34 of the
#     RHEL 9 family and Amazon Linux 2023 — both define
#     `_dl_find_object@@GLIBC_2.35` while `ldd --version` reports 2.34 (checked
#     on AlmaLinux 9.8). GCC 14's libgcc_s.so.1 references it because whether
#     libgcc takes the fast unwind lookup is decided by the headers of the
#     system GCC was BUILT on, i.e. this image; the reference then travels with
#     the library into the release bundle, where the build image's own choice
#     becomes the release's requirement. Ubuntu 22.04 (2.35) and Debian 12
#     (2.36) have the symbol natively, so every platform the release documents
#     is covered either way.
#
# Exempted by NAME, never by version: "2.34, plus whatever node the image
# happens to need" is the reflex that turns an allowlist into a rubber stamp,
# and it would swallow the next node just as quietly. For the same reason the
# exemption is scoped to the STAGING tree — only a bundled third-party runtime
# may carry a backported-node reference, and the project's own libraries are
# scanned twice (build tree and staging tree), so a reference of ours that
# bound this node would still fail the build-tree pass.
set(_backported_refs "_dl_find_object@GLIBC_2.35")
set(_n_backported 0)

set(_failures "")
set(_max_glibc "0.0")
set(_max_glibcxx 0)
set(_max_cxxabi 0)
set(_max_openssl "0.0.0")

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

    # Only a bundled runtime library may carry a backported-node reference; see
    # _backported_refs above. string(FIND) rather than a regex, because a staging
    # path can contain regex metacharacters.
    set(_may_exempt FALSE)
    if(DEFINED STAGING_DIR)
        string(FIND "${_art}" "${STAGING_DIR}/" _stage_pos)
        if(_stage_pos EQUAL 0)
            set(_may_exempt TRUE)
        endif()
    endif()

    set(_over_syms "")
    foreach(_ref ${_refs})
        if(_may_exempt AND _ref IN_LIST _backported_refs)
            math(EXPR _n_backported "${_n_backported} + 1")
            continue()
        endif()
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

    # --- C++ ABI symbol versions ---------------------------------------------
    # The header comment claimed this all along and the code did not do it.
    # CXXABI_1.3.x is a separate version namespace from GLIBCXX_3.4.x, carrying
    # the Itanium ABI entry points — typeinfo, exception handling, the
    # __cxa_* family. It moves independently: a tree can bind a new CXXABI node
    # while its highest GLIBCXX stays put.
    string(REGEX MATCHALL "@CXXABI_1\\.3\\.[0-9]+" _abi_refs "${_out}")
    list(REMOVE_DUPLICATES _abi_refs)
    foreach(_ref ${_abi_refs})
        string(REGEX REPLACE "^@CXXABI_1\\.3\\." "" _minor "${_ref}")
        if(_minor GREATER _max_cxxabi)
            set(_max_cxxabi ${_minor})
        endif()
        if(DEFINED CXXABI_FLOOR)
            if(_minor GREATER ${CXXABI_FLOOR})
                list(APPEND _failures
                    "${_name}: requires CXXABI_1.3.${_minor} > floor 1.3.${CXXABI_FLOOR}")
            endif()
        endif()
    endforeach()

    # --- OpenSSL symbol versions ---------------------------------------------
    # The gap a SONAME allowlist cannot close. OpenSSL 3 keeps one SONAME
    # (libssl.so.3) across every 3.x release but versions its symbols per minor:
    # an API added in 3.2 binds OPENSSL_3.2.0. Building against 3.5 and running
    # on Ubuntu 22.04's 3.0.2 therefore fails at load with DT_NEEDED looking
    # perfectly correct. Enforcing the node keeps a future source change from
    # raising the OpenSSL requirement silently.
    string(REGEX MATCHALL
        "[A-Za-z_][A-Za-z0-9_.]*@OPENSSL_[0-9]+\\.[0-9]+\\.[0-9]+" _ssl_refs "${_out}")
    list(REMOVE_DUPLICATES _ssl_refs)
    set(_over_ssl "")
    foreach(_ref ${_ssl_refs})
        string(REGEX REPLACE "^.*@OPENSSL_" "" _ver "${_ref}")
        _version_gt("${_ver}" "${_max_openssl}" _newer)
        if(_newer)
            set(_max_openssl "${_ver}")
        endif()
        if(DEFINED OPENSSL_FLOOR)
            _version_gt("${_ver}" "${OPENSSL_FLOOR}" _over)
            if(_over)
                list(APPEND _over_ssl "${_ref}")
            endif()
        endif()
    endforeach()
    if(_over_ssl)
        list(SORT _over_ssl)
        list(JOIN _over_ssl ", " _ssl_text)
        list(APPEND _failures
            "${_name}: requires OpenSSL newer than floor ${OPENSSL_FLOOR}: ${_ssl_text}")
    endif()

    # --- DT_NEEDED allowlist -------------------------------------------------
    if(STRICT_NEEDED)
        string(REGEX MATCHALL "Shared library: \\[[^]]*\\]" _needed "${_out}")
        foreach(_line ${_needed})
            string(REGEX REPLACE ".*\\[([^]]*)\\]" "\\1" _lib "${_line}")
            # _own_libs is the set of SONAMEs present in the staging tree, so
            # this reads as: either the host is expected to provide it
            # (allowlist) or the release actually ships it (staged).
            list(FIND _allowed_needed "${_lib}" _idx)
            list(FIND _own_libs "${_lib}" _own_idx)
            # No libboost_* prefix exemption anymore. It existed because the
            # build tree could not tell which Boost components ship; the
            # staging tree can, so a blanket prefix match would now only serve
            # to accept a Boost library the release forgot to bundle.
            if(_idx EQUAL -1 AND _own_idx EQUAL -1)
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
    "portability floor: ${_n} artifacts scanned, highest requirements — "
    "GLIBC_${_max_glibc} (floor ${GLIBC_FLOOR}), GLIBCXX_3.4.${_max_glibcxx}, "
    "CXXABI_1.3.${_max_cxxabi}, OPENSSL_${_max_openssl}")

# Reported, not silent: an exemption that starts spreading across artifacts has
# to be visible in the log of a passing run, because a passing run is exactly
# when nobody reads the file.
if(_n_backported GREATER 0)
    message(STATUS
        "portability floor: ${_n_backported} backported-node reference(s) exempted "
        "(bundled runtime only): ${_backported_refs}")
endif()
