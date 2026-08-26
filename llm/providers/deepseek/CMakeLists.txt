# =============================================================================
# deepseek — provider plugin (llm package)
#
# The first provider on the provider-neutral chat-completions adapter
# (llm_chat_completions): DeepSeek's endpoint defaults, thinking-mode request
# policy, reasoning_content replay requirement, and cache-hit usage spelling
# live in the header-only dialect (include/llm/deepseek/dialect.hpp); the
# plugin .cpp itself is only the model subclass and the two standard factory
# aliases. Emitted next to llm_openai under <exe>/plugins/llm.
#
# The provider-local tests exercise the dialect (offline) and the full
# converse() exchange against a loopback server, so this directory needs the
# test subdirectory gated like every other module.
# =============================================================================
add_library(llm_deepseek MODULE src/deepseek_plugin.cpp)
target_include_directories(llm_deepseek PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(llm_deepseek PRIVATE llm_chat_completions boost_dll_iface)
target_compile_options(llm_deepseek PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
set_target_properties(llm_deepseek PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/llm")
if(LTO_SUPPORT)
    set_target_properties(llm_deepseek PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
