add_executable(test_qwen_plugin test_qwen_plugin.cpp)
target_include_directories(test_qwen_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_qwen_plugin PRIVATE
    llm_iface llm_chat_completions endpoint_lib eventbus_iface
    tools_intrinsic_modality_assist fileio_lib Boost::unit_test_framework)
target_compile_definitions(test_qwen_plugin PRIVATE
    QWEN_PLUGIN_DIR="$<TARGET_FILE_DIR:llm_qwen>"
    QWEN_IMAGE_FIXTURE="${PROJECT_SOURCE_DIR}/utils/fileio/test/fixtures/pixel.png"
    QWEN_JPEG_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/pixel.jpg")
set_target_properties(test_qwen_plugin PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(test_qwen_plugin llm_qwen)
add_test(NAME test_qwen_plugin COMMAND test_qwen_plugin)
set_tests_properties(test_qwen_plugin PROPERTIES TIMEOUT 60)
