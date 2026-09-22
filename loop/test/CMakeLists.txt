add_executable(test_loop test_loop.cpp)
target_link_libraries(test_loop PRIVATE loop_lib Boost::unit_test_framework)
target_compile_options(test_loop PRIVATE -pthread)
add_test(NAME test_loop COMMAND test_loop)
set_tests_properties(test_loop PROPERTIES TIMEOUT 30)

add_executable(test_loop_model_cancellation test_model_cancellation.cpp)
target_link_libraries(test_loop_model_cancellation PRIVATE
    loop_lib
    llm_chat_completions
    llm_responses
    Boost::unit_test_framework)
add_test(NAME test_loop_model_cancellation COMMAND test_loop_model_cancellation)
set_tests_properties(test_loop_model_cancellation PROPERTIES TIMEOUT 30)
