# =============================================================================
# plugin_boundary_hygiene.cmake — structural assertions for the hardened
# plugin boundary (run as a ctest via `cmake -P`; zero dependencies beyond
# nm/readelf, both from binutils).
#
# The live-object plugin contract is safe under the same-execution-context
# strategy (docs/abi-context.md) only while its supporting structures hold.
# This script turns each of them into a checkable fact:
#
#   1. Plugin exports present — every provider .so must export both factory
#      aliases (create_llm_plugin / create_llm_model). Presence only: a live
#      plugin legitimately carries model-subclass symbols, so the export set
#      is not locked.
#   2. Shared-stack binding — each plugin's DT_NEEDED must bind its protocol
#      adapter (libllm_chat_completions.so / libllm_responses.so) and the
#      shared asio runtime by SONAME. Reverting an adapter to a static copy
#      (or a plugin silently embedding one) drops the entry and fails here.
#      Every other NEEDED entry must be on the allowlist (C runtime + the
#      project's shared runtime family).
#   3. Host export unification — the host executable is built with
#      ENABLE_EXPORTS ON (-rdynamic) so its emissions of the header-only
#      contract classes' typeinfo (`_ZTI…ExtensionContext`, `_ZTI…LLMModel…`)
#      are exported and become the authoritative binding targets for plugins.
#   4. Strong-symbol unification — the shared protocol adapter defines the
#      model/exception classes' typeinfo exactly once (`_ZTI…ChatCompletion…`),
#      which is what makes cross-boundary catches match by type identity.
#
# Inputs (all required, passed via -D on the command line):
#   DEEPSEEK_SO / OPENAI_SO  full paths to the provider plugin modules
#   HOST_EXE                 full path to a host built with ENABLE_EXPORTS ON
#   CHAT_LIB / RESPONSES_LIB full paths to the shared protocol adapters
# =============================================================================
cmake_minimum_required(VERSION 3.20)

foreach(_req IN ITEMS DEEPSEEK_SO OPENAI_SO HOST_EXE CHAT_LIB RESPONSES_LIB)
    if(NOT DEFINED ${_req})
        message(FATAL_ERROR "plugin_boundary_hygiene: ${_req} must be defined")
    endif()
endforeach()

find_program(NM_EXECUTABLE NAMES nm REQUIRED)
find_program(READELF_EXECUTABLE NAMES readelf REQUIRED)

# The complete set of shared objects a provider plugin may depend on: the C
# runtime plus the project's own shared runtime family. Anything else in
# DT_NEEDED means a plugin grew a private runtime — a boundary regression.
set(_allowed_needed
    "libasio.so" "libeventbus.so" "liblogging.so"
    "libllm_chat_completions.so" "libllm_responses.so"
    "libssl.so.3" "libcrypto.so.3"
    "libstdc++.so.6" "libm.so.6" "libgcc_s.so.1" "libc.so.6"
    "ld-linux-x86-64.so.2")

set(_failures "")

function(_check_plugin so required_needed)
    # --- factory aliases exported ------------------------------------------------
    execute_process(
        COMMAND ${NM_EXECUTABLE} -D --defined-only "${so}"
        OUTPUT_VARIABLE _nm RESULT_VARIABLE _nm_res)
    if(NOT _nm_res EQUAL 0)
        list(APPEND _failures "nm failed on ${so}")
        return()
    endif()
    foreach(_alias IN ITEMS create_llm_plugin create_llm_model simplex_plugin_magic)
        if(NOT _nm MATCHES "[^A-Za-z0-9_]${_alias}($|[^A-Za-z0-9_])")
            list(APPEND _failures "${so}: required export ${_alias} not present")
        endif()
    endforeach()

    # --- DT_NEEDED: required present, everything else allowlisted ----------------
    execute_process(
        COMMAND ${READELF_EXECUTABLE} -d "${so}"
        OUTPUT_VARIABLE _dyn RESULT_VARIABLE _rf_res)
    if(NOT _rf_res EQUAL 0)
        list(APPEND _failures "readelf failed on ${so}")
        return()
    endif()
    foreach(_lib ${${required_needed}})
        if(NOT _dyn MATCHES "Shared library: \\[${_lib}\\]")
            list(APPEND _failures "${so}: missing DT_NEEDED ${_lib}")
        endif()
    endforeach()
    string(REGEX MATCHALL "Shared library: \\[[^]]*\\]" _needed_lines "${_dyn}")
    foreach(_line ${_needed_lines})
        string(REGEX REPLACE ".*\\[([^]]*)\\]" "\\1" _lib "${_line}")
        list(FIND _allowed_needed "${_lib}" _idx)
        if(_idx EQUAL -1
           AND NOT _lib MATCHES "^libboost_") # Boost shared runtimes (link-wide policy)
            list(APPEND _failures "${so}: unexpected DT_NEEDED ${_lib}")
        endif()
    endforeach()
    # Propagate failures out of the function scope.
    set(_failures "${_failures}" PARENT_SCOPE)
endfunction()

# --- 1 + 2: the two bundled provider plugins ---------------------------------
set(_deepseek_needed "libllm_chat_completions.so;libasio.so")
_check_plugin("${DEEPSEEK_SO}" _deepseek_needed)
set(_openai_needed "libllm_responses.so;libasio.so")
_check_plugin("${OPENAI_SO}" _openai_needed)

# --- 3: the host exports the contract classes' typeinfo ----------------------
execute_process(
    COMMAND ${NM_EXECUTABLE} -D --defined-only "${HOST_EXE}"
    OUTPUT_VARIABLE _host_nm RESULT_VARIABLE _host_res)
if(NOT _host_res EQUAL 0)
    list(APPEND _failures "nm failed on ${HOST_EXE}")
else()
    if(NOT _host_nm MATCHES "_ZTI[A-Za-z0-9_]*ExtensionContext")
        list(APPEND _failures
            "${HOST_EXE}: no ExtensionContext typeinfo exported — ENABLE_EXPORTS missing?")
    endif()
    if(NOT _host_nm MATCHES "_ZTI[A-Za-z0-9_]*LLMModel")
        list(APPEND _failures
            "${HOST_EXE}: no LLMModel typeinfo exported — ENABLE_EXPORTS missing?")
    endif()
endif()

# --- 4: the shared adapters own the protocol classes' typeinfo ---------------
foreach(_pair "CHAT_LIB:ChatCompletions(Model|ApiException)" "RESPONSES_LIB:Responses(Model|ApiException)")
    string(REPLACE ":" ";" _fields "${_pair}")
    list(GET _fields 0 _var)
    list(GET _fields 1 _class_re)
    execute_process(
        COMMAND ${NM_EXECUTABLE} -D --defined-only "${${_var}}"
        OUTPUT_VARIABLE _lib_nm RESULT_VARIABLE _lib_res)
    if(NOT _lib_res EQUAL 0)
        list(APPEND _failures "nm failed on ${${_var}}")
    elseif(NOT _lib_nm MATCHES "_ZTI[A-Za-z0-9_]*(${_class_re})")
        list(APPEND _failures
            "${${_var}}: no ${_class_re} typeinfo defined — adapter no longer a shared lib?")
    endif()
endforeach()

if(_failures)
    message(FATAL_ERROR "plugin boundary hygiene FAILED:\n  - ${_failures}")
endif()
message(STATUS "plugin boundary hygiene: all structural assertions passed")
