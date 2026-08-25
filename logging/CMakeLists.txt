# =============================================================================
# logging — module
#
# A standalone global module: a thread-safe console logger used across the whole
# project, not just indextools. Split out of indextools/ so any module can log
# without pulling in the indextools host library.
#
# Produces `logging_lib` (STATIC), whose PUBLIC include dir exposes
# logging/logger.hpp (included as "logging/logger.hpp"). The Logger class lives
# in namespace `logging`. It depends only on the C++ standard library.
#
# Header management is interface-target-only: consumers link `logging_lib` and
# get its include dir transitively (PUBLIC). No directory-scope includes, no
# propagated path variables.
# =============================================================================

set(LOGGING_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ========================================== #
# Build logging_lib (static library)         #
# ========================================== #
set(LIB_NAME "logging_lib")

add_library(${LIB_NAME} STATIC
    "${LOGGING_SOURCE_DIR}/src/logger.cpp"
)
set_target_properties(${LIB_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(${LIB_NAME} PUBLIC
    "${LOGGING_SOURCE_DIR}/include"
)
target_compile_options(${LIB_NAME} PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
# Expose the build's debug status as a PUBLIC compile definition so that
# logger.hpp — included by consumers — can seed the compile-time constant
# kDebugBuild without any macros at call sites.  LOGGING_DEBUG is treated as a
# plain flag (defined = on), so it is emitted bare, only in the Debug
# configuration; in every other configuration it is simply absent.
target_compile_definitions(${LIB_NAME} PUBLIC
    $<$<CONFIG:Debug>:LOGGING_DEBUG>
)
# No external link deps — the logger uses only the standard library.
if(LTO_SUPPORT)
    set_target_properties(${LIB_NAME} PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
    )
endif()

# ========================================== #
# Unit tests                                 #
# ========================================== #
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
