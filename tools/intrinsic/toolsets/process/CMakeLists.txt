# =============================================================================
# process — intrinsic toolset (tools/intrinsic package)
#
# Process management as a tool family: the four calls a model makes to run a
# child process, feed it, read its output incrementally, and wait for one of
# them to finish.
# It joins the invocation layer (tools/) to process/'s single-child manager
# (ProcessHandle) with the piece neither owns — a table of live children a model
# can name across turns.
#
#   process/session_store.hpp  the session table: readable ids that are never
#                              reused, a strand per child, the per-stream read
#                              cursors that make "what is new since I last
#                              looked" answerable, reaping, and the
#                              terminate_all() shutdown path;
#   process/tools.hpp          the four ToolInterface implementations and the
#                              ProcessToolBase that adds this family's session
#                              id argument to the shared IntrinsicTool;
#   process/toolset.hpp        the ToolSet a host registers with its
#                              ToolRegistry;
#   process/schemas.hpp        where the tools' declarations live, and the one
#                              place that answers it.
#
# Links the shared core (tools_intrinsic) for the argument/result/confirmation
# machinery, the toolset routing and the declaration loader, and process_lib for
# the manager itself. SHARED for the same typeinfo reason as the core — see the
# parent CMakeLists.
#
# THE DECLARATIONS THEMSELVES are schemas/*.yaml, one file per tool, loaded at
# run time by the tool that names it — plus schemas/skill.yaml, the set's own
# guidance for a model (how the four are used together), loaded by the set. They
# are listed as target sources for the sake of an IDE and of `make`'s dependency
# tracking only — nothing is compiled from them, and editing one takes effect on
# the next run without a rebuild.
#
# The directory is handed to the library as the SIMPLEX_PROCESS_SCHEMA_DIR
# macro, which is the development/test answer to "where are the declarations?":
# the source tree they were written in. A deployment that ships the binaries
# without the sources sets the environment variable of the same name instead,
# and process/schemas.hpp explains both (checked and reported per file, so a
# directory that is not there costs one tool and one error line, never the
# host).
#
# Note on ordering: process_lib comes from a module added AFTER tools/ at the
# top level. That is fine — CMake resolves target link names at generate time,
# not at add_subdirectory() time.
# =============================================================================
add_library(tools_intrinsic_process SHARED
    src/schemas.cpp
    src/session_store.cpp
    src/tools.cpp
    src/toolset.cpp
    # Not compiled: the model-facing declarations, one file per tool.
    schemas/poll_process.yaml
    schemas/read_process.yaml
    schemas/send_process.yaml
    schemas/spawn_process.yaml
    # Not compiled either: the set's skill, which is about using all four.
    schemas/skill.yaml
)
target_include_directories(tools_intrinsic_process PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic_process PUBLIC
    tools_intrinsic
    process_lib
)
target_compile_definitions(tools_intrinsic_process PRIVATE
    SIMPLEX_PROCESS_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/schemas")

# ---- Release layout ---------------------------------------------------------
# A staged release carries the declarations — and the set's skill.yaml with
# them, since the whole directory is installed — BESIDE ITS EXECUTABLES
# (<prefix>/bin/schemas/process), which is the second place
# process/schemas.hpp looks — the same "relative to the executable"
# convention the host already uses for its plugins, so the installed tree
# needs no path configuration to find them.
#
# The destination is spelled out rather than taken from CMAKE_INSTALL_BINDIR
# deliberately: GNUInstallDirs is included by cmake/SimplexRelease.cmake, which
# the top level includes AFTER this package (it walks targets, so they must
# exist first) and which pins the library directory to `lib` before that
# include. Pulling GNUInstallDirs in here would set CMAKE_INSTALL_LIBDIR
# first and leave that pinning a no-op on RHEL-family distros.
install(DIRECTORY schemas/ DESTINATION bin/schemas/process)
target_compile_options(tools_intrinsic_process PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
set_target_properties(tools_intrinsic_process PROPERTIES
    OUTPUT_NAME intrinsic_process_tools)
if(LTO_SUPPORT)
    set_target_properties(tools_intrinsic_process PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
