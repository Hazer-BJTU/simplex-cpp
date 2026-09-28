add_executable(test_modality_assist_tools test_tools.cpp)
target_link_libraries(test_modality_assist_tools PRIVATE
    tools_intrinsic_modality_assist fileio_lib llm_chat_completions llm_responses
    Boost::unit_test_framework)
target_compile_definitions(test_modality_assist_tools PRIVATE
    MODALITY_IMAGE_FIXTURE="${PROJECT_SOURCE_DIR}/utils/fileio/test/fixtures/pixel.png")
add_test(NAME test_modality_assist_tools COMMAND test_modality_assist_tools)
set_tests_properties(test_modality_assist_tools PROPERTIES TIMEOUT 30)
