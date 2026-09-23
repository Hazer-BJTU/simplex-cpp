add_executable(test_loop test_loop.cpp)
target_link_libraries(test_loop PRIVATE loop_lib Boost::unit_test_framework)
target_compile_options(test_loop PRIVATE -pthread)
add_test(NAME test_loop COMMAND test_loop)
set_tests_properties(test_loop PROPERTIES TIMEOUT 30)

add_executable(test_loop_hook_interface test_hook_interface.cpp)
target_link_libraries(test_loop_hook_interface PRIVATE
    loop_lib Boost::unit_test_framework)
add_test(NAME test_loop_hook_interface COMMAND test_loop_hook_interface)

add_executable(test_loop_hook_registry test_hook_registry.cpp)
target_link_libraries(test_loop_hook_registry PRIVATE
    loop_lib Boost::unit_test_framework)
add_test(NAME test_loop_hook_registry COMMAND test_loop_hook_registry)

add_executable(test_loop_model_cancellation test_model_cancellation.cpp)
target_link_libraries(test_loop_model_cancellation PRIVATE
    loop_lib
    llm_chat_completions
    llm_responses
    Boost::unit_test_framework)
add_test(NAME test_loop_model_cancellation COMMAND test_loop_model_cancellation)
set_tests_properties(test_loop_model_cancellation PROPERTIES TIMEOUT 30)

add_executable(test_loop_example_terminal test_example_terminal.cpp)
target_link_libraries(test_loop_example_terminal PRIVATE Boost::unit_test_framework)
add_test(NAME test_loop_example_terminal COMMAND test_loop_example_terminal)

# The fake provider launches real process tools: execute only in a container.
# Resolve Python when CTest runs, not when CMake configures: the portable build
# tree is also tested on older distros where the build image's /usr/bin/python3.9
# does not exist. The launcher returns 77 when Python or Docker is absent.
add_test(NAME loop_example_container_smoke
    COMMAND /bin/sh ${CMAKE_CURRENT_SOURCE_DIR}/run_example_container_smoke.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/test_example_container.py
            $<TARGET_FILE:loop_deepseek_chat>)
set_tests_properties(loop_example_container_smoke PROPERTIES
    TIMEOUT 45 SKIP_RETURN_CODE 77)
