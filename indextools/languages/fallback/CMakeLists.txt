# ============================================================================ #
# Fallback language module                                                     #
# ============================================================================ #
#
# The generic tokenizer-only analyzer used for any extension without a
# dedicated language. Self-contained and auto-discovered by ../CMakeLists.txt.
# Unlike python/, it needs no third-party parser, so it has no dependency probe.

set(LANG_NAME        "fallback")
set(LANG_ANALYZER    "fallback_analyzer")
set(LANG_PLUGIN      "langfallback")   # -> liblangfallback.so

set(_compile_opts
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# ------------------------------------------------------- static analyzer library
add_library(${LANG_ANALYZER} STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fallbacklang.cpp"
)
target_include_directories(${LANG_ANALYZER} PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(${LANG_ANALYZER} PRIVATE
    "${INDEXTOOLS_INCLUDE_DIR}"
    "${SIMPLEX_THIRDPARTY_DIR}/include"
)
target_compile_options(${LANG_ANALYZER} PRIVATE ${_compile_opts})
set_target_properties(${LANG_ANALYZER} PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------- runtime plugin .so
add_library(${LANG_PLUGIN} MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fallbacklang_plugin.cpp"
)
target_include_directories(${LANG_PLUGIN} PRIVATE
    "${INDEXTOOLS_INCLUDE_DIR}"
    "${SIMPLEX_THIRDPARTY_DIR}/include"
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
