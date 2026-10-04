# Thin provider plugin on the shared Chat Completions runtime.
add_library(llm_qwen MODULE src/qwen_plugin.cpp)
target_include_directories(llm_qwen PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(llm_qwen PUBLIC llm_chat_completions boost_dll_iface)
target_compile_options(llm_qwen PRIVATE $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)
set_target_properties(llm_qwen PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/llm")
if(LTO_SUPPORT)
    set_target_properties(llm_qwen PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
