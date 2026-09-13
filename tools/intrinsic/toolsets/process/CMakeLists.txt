# =============================================================================
# process — intrinsic toolset (tools/intrinsic package)
#
# Process management as a tool family: the six calls a model makes to run a
# child process, feed it, read its output incrementally, wait on it and kill it.
# It joins the invocation layer (tools/) to process/'s single-child manager
# (ProcessHandle) with the piece neither owns — a table of live children a model
# can name across turns.
#
#   process/session_store.hpp  the session table: readable ids that are never
#                              reused, a strand per child, the per-stream read
#                              cursors that make "what is new since I last
#                              looked" answerable, reaping, and the
#                              terminate_all() shutdown path;
#   process/tools.hpp          the six ToolInterface implementations and the
#                              ProcessToolBase that adds this family's session
#                              id argument to the shared IntrinsicTool;
#   process/toolset.hpp        the ToolSet a host registers with its
#                              ToolRegistry.
#
# Links the shared core (tools_intrinsic) for the argument/result/confirmation
# machinery and the toolset routing, and process_lib for the manager itself.
# SHARED for the same typeinfo reason as the core — see the parent CMakeLists.
#
# Note on ordering: process_lib comes from a module added AFTER tools/ at the
# top level. That is fine — CMake resolves target link names at generate time,
# not at add_subdirectory() time.
# =============================================================================
add_library(tools_intrinsic_process SHARED
    src/session_store.cpp
    src/tools.cpp
    src/toolset.cpp
)
target_include_directories(tools_intrinsic_process PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic_process PUBLIC
    tools_intrinsic
    process_lib
)
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
