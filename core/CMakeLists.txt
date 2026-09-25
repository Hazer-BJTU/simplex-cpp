configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/prompts/coding_agent.yaml"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/prompts/coding_agent.yaml"
    COPYONLY)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/prompts/coding_agent.yaml"
    DESTINATION bin/prompts)

add_library(core_protocol SHARED src/protocol.cpp)
target_include_directories(core_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(core_protocol PUBLIC dataclass_iface Boost::headers)
set_target_properties(core_protocol PROPERTIES OUTPUT_NAME simplex_core_protocol)
add_library(core_lib SHARED src/application.cpp src/confirmation.cpp)
target_include_directories(core_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(core_lib PUBLIC core_protocol load_lib loop_lib intercom_exchange)
target_link_libraries(core_lib PRIVATE simplex_versioning fileio_lib tools_intrinsic_process tools_intrinsic_reading async_eventbus_iface loop_intrinsic_context_statistic)
set_target_properties(core_lib PROPERTIES OUTPUT_NAME simplex_core)
add_executable(simplex_worker src/main.cpp)
target_link_libraries(simplex_worker PRIVATE core_lib Boost::program_options)
add_dependencies(simplex_worker llm_openai llm_deepseek)
set_target_properties(simplex_worker PROPERTIES ENABLE_EXPORTS ON)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
add_subdirectory(example)
