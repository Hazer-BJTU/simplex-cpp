# Interactive host for loop::run; real process tools are exercised in Docker.
add_executable(loop_deepseek_chat deepseek_chat.cpp)
target_link_libraries(loop_deepseek_chat PRIVATE
    loop_lib tools_intrinsic_process async_eventbus_iface)
set_target_properties(loop_deepseek_chat PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(loop_deepseek_chat llm_deepseek)

if(BUILD_TESTS)
    add_test(NAME loop_deepseek_chat_catalogue COMMAND loop_deepseek_chat --tools)
    set_tests_properties(loop_deepseek_chat_catalogue PROPERTIES TIMEOUT 30)
endif()
