add_library(llm_openai MODULE src/openai_plugin.cpp)
target_link_libraries(llm_openai PRIVATE llm_responses boost_dll_iface)
target_compile_options(llm_openai PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
set_target_properties(llm_openai PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/llm")
if(LTO_SUPPORT)
    set_target_properties(llm_openai PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()
