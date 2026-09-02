# =============================================================================
# logging — module (utils package)
#
# A standalone global module: a thread-safe console logger used across the whole
# project, not just indextools. Lives under the utils/ package; the target and
# include paths are unchanged from the standalone-module era, so consumers keep
# linking `logging_lib` and including "logging/logger.hpp".
#
# Produces `logging_lib` (SHARED, output name `logging` → liblogging.so), whose
# PUBLIC include dir exposes logging/logger.hpp (included as
# "logging/logger.hpp"). The Logger class lives in namespace `logging`. It
# depends only on the C++ standard library.
#
# Header management is interface-target-only: consumers link `logging_lib` and
# get its include dir transitively (PUBLIC). No directory-scope includes, no
# propagated path variables.
# =============================================================================

set(LOGGING_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ========================================== #
# Build logging_lib (shared library)         #
# ========================================== #
# SHARED for the same reason as libasio/libeventbus (utils/asio, utils/eventbus):
# the Logger's static mutex and level state must exist exactly once per
# process. As a static archive, every executable and every dlopened plugin
# linked its own copy — a host-side level change would leave plugins logging
# at the old level, and each module would serialize on its own mutex. The
# SONAME makes uniqueness a loader guarantee.
add_library(logging_lib SHARED
    "${LOGGING_SOURCE_DIR}/src/logger.cpp"
)
set_target_properties(logging_lib PROPERTIES OUTPUT_NAME logging)
target_include_directories(logging_lib PUBLIC
    "${LOGGING_SOURCE_DIR}/include"
)
target_compile_options(logging_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
# Expose the build's debug status as a PUBLIC compile definition so that
# logger.hpp — included by consumers — can seed the compile-time constant
# kDebugBuild without any macros at call sites.  LOGGING_DEBUG is treated as a
# plain flag (defined = on), so it is emitted bare, only in the Debug
# configuration; in every other configuration it is simply absent.
target_compile_definitions(logging_lib PUBLIC
    $<$<CONFIG:Debug>:LOGGING_DEBUG>
)
# No external link deps — the logger uses only the standard library.
if(LTO_SUPPORT)
    set_target_properties(logging_lib PROPERTIES
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
