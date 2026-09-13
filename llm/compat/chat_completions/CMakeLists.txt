# =============================================================================
# chat_completions — the OpenAI-compatible Chat Completions adapter
#
# One of the two protocol adapters of the compatibility layer (see
# ../README.md): the canonical-data-to-wire mapping for the Chat Completions
# protocol, its SSE stream handler, its reader, and the dialect seam a provider
# narrows. Providers may derive from ChatCompletionsModel and inject a dialect;
# this target itself intentionally exports no plugin.
#
# PUBLIC include directory = <this dir>/include, which holds
# llm/compat/chat_completions/*.hpp — the layer appears in the logical path, so
# a consumer writes
#   #include "llm/compat/chat_completions/model.hpp"
# and the include path says which layer the header belongs to. What did NOT
# change is the namespace (llm::chat_completions), the target name and the
# SONAME; what DID is that the target, not the module-wide llm_iface, is what a
# consumer links for it. See ../README.md.
# =============================================================================
add_library(llm_chat_completions SHARED
    src/model.cpp
    src/interpreter.cpp
    src/stream_handler.cpp
    src/reader.cpp
)
target_include_directories(llm_chat_completions PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
# eventbus_iface: converse() broadcasts live reasoning increments on the
# process-wide bus (PUBLIC — the adapter publishes through default_bus(),
# whose singleton lives in the SHARED eventbus library; subscribers want the
# headers anyway).
target_link_libraries(llm_chat_completions
    PUBLIC llm_iface endpoint_lib eventbus_iface)
target_compile_options(llm_chat_completions PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
if(LTO_SUPPORT)
    set_target_properties(llm_chat_completions PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
