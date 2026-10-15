add_executable(test_custom_chat test_custom_chat.cpp)
target_include_directories(test_custom_chat PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_custom_chat PRIVATE llm_chat_completions Boost::unit_test_framework)
target_compile_definitions(test_custom_chat PRIVATE
    CUSTOM_CHAT_PLUGIN_DIR="$<TARGET_FILE_DIR:llm_custom_chat>")
set_target_properties(test_custom_chat PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(test_custom_chat llm_custom_chat)
add_test(NAME test_custom_chat COMMAND test_custom_chat)
set_tests_properties(test_custom_chat PROPERTIES TIMEOUT 60)
