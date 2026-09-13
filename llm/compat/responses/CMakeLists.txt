# =============================================================================
# responses — the compiled Responses-API adapter
#
# The other protocol adapter of the compatibility layer (see ../README.md), and
# the one the module ships a model for: the Responses-API event stream, its
# typed deltas, the interpreter that maps model_io onto the wire, and the reader
# that assembles a MessageItem back. llm_openai is the plugin exposing it.
#
# PUBLIC include directory = <this dir>/include, which holds
# llm/compat/responses/*.hpp — the layer appears in the logical path, so a
# consumer writes
#   #include "llm/compat/responses/model.hpp"
# and the include path says which layer the header belongs to. What did NOT
# change is the namespace (llm::responses), the target name and the SONAME; what
# DID is that the target, not the module-wide llm_iface, is what a consumer
# links for it. See ../README.md.
# =============================================================================
add_library(llm_responses SHARED
    src/model.cpp
    src/interpreter.cpp
    src/stream_handler.cpp
    src/reader.cpp
)
target_include_directories(llm_responses PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(llm_responses PUBLIC llm_iface endpoint_lib)
target_compile_options(llm_responses PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
if(LTO_SUPPORT)
    set_target_properties(llm_responses PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
