# ============================================================================ #
# Python language module                                                       #
# ============================================================================ #
#
# Self-contained: this file owns everything Python-specific. It is discovered
# automatically by ../CMakeLists.txt — nothing above needs editing to build it.
# Use it as the template for new languages (see ../README.md).

set(LANG_NAME        "python")   # module id (also the plugin filename stem)
set(LANG_ANALYZER    "python_analyzer")     # static lib used by unit tests
set(LANG_PLUGIN      "langpython")          # runtime plugin -> liblangpython.so

# ------------------------------------------------------------------ dependencies
# Python analysis is tree-sitter based; each language finds its own deps so no
# shared dependency list exists at the parent level.
set(_deps "")
foreach(_lib "tree-sitter" "tree-sitter-python")
    find_library(${_lib}_TARGET NAMES ${_lib} PATHS "${SIMPLEX_THIRDPARTY_DIR}/libs")
    if(NOT ${_lib}_TARGET)
        message(FATAL_ERROR "[${LANG_NAME}] third party library ${_lib} not found")
    endif()
    list(APPEND _deps ${${_lib}_TARGET})
endforeach()

set(_compile_opts
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# ------------------------------------------------------- static analyzer library
# The analyzer implementation, minus the plugin export wrapper. Unit tests that
# include "pythonlang.hpp" link this; the include dir is PUBLIC so linking it is
# enough to get the header. indextools_iface is also PUBLIC because pythonlang.hpp
# #includes lang.hpp (and lang.hpp pulls in nlohmann/json), so the indextools
# contract headers are part of this lib's public surface. PIC so it can also
# fold into the plugin .so.
add_library(${LANG_ANALYZER} STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pythonlang.cpp"
)
target_include_directories(${LANG_ANALYZER} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_options(${LANG_ANALYZER} PRIVATE ${_compile_opts})
target_link_libraries(${LANG_ANALYZER} PUBLIC indextools_iface ${_deps})
set_target_properties(${LANG_ANALYZER} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------- runtime plugin .so
# Bundles the analyzer + the export wrapper (which defines create_lang_plugin via
# BOOST_DLL_ALIAS) into a MODULE loaded at runtime from bin/plugins/. The indextools
# contract headers (lang_plugin.hpp) arrive transitively via ${LANG_ANALYZER}'s
# PUBLIC link to indextools_iface.
add_library(${LANG_PLUGIN} MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pythonlang_plugin.cpp"
)
target_compile_options(${LANG_PLUGIN} PRIVATE ${_compile_opts})
target_link_libraries(${LANG_PLUGIN} PRIVATE ${LANG_ANALYZER} boost_dll_iface)
set_target_properties(${LANG_PLUGIN} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins"
)

if(LTO_SUPPORT)
    set_target_properties(${LANG_ANALYZER} ${LANG_PLUGIN} PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
    )
endif()

# ------------------------------------------------------------------ registration
set_property(GLOBAL APPEND PROPERTY INDEX_LANG_PLUGINS ${LANG_PLUGIN})
set_property(GLOBAL APPEND PROPERTY INDEX_LANG_ANALYZER_LIBS ${LANG_ANALYZER})
