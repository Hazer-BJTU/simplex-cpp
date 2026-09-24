add_executable(test_load_plugins test_plugins.cpp)
target_link_libraries(test_load_plugins PRIVATE
    load_lib yamlconfig_iface Boost::unit_test_framework)
set_target_properties(test_load_plugins PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(test_load_plugins PRIVATE
    LOAD_TEMPLATE="${PROJECT_SOURCE_DIR}/load/config.example.yaml"
    LOAD_OPENAI_MODULE="$<TARGET_FILE:llm_openai>"
    LOAD_DEEPSEEK_MODULE="$<TARGET_FILE:llm_deepseek>"
    LOAD_TOOL_MODULE="$<TARGET_FILE:tools_extension_noop>"
    LOAD_TOOL_SCHEMA="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/schemas/tools/extensions/noop_toolset"
    LOAD_FIXTURE_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}/fixtures")
add_dependencies(test_load_plugins
    llm_openai llm_deepseek tools_extension_noop loop_extension_noop)

foreach(name first second unselected)
    add_library(load_hook_fixture_${name} MODULE hook_fixture.cpp)
    target_link_libraries(load_hook_fixture_${name} PRIVATE
        loop_extensions boost_dll_iface)
    target_compile_definitions(load_hook_fixture_${name} PRIVATE
        LOAD_FIXTURE_NAME="${name}"
        LOAD_FIXTURE_NAMESPACE=load_fixture_${name})
    set_target_properties(load_hook_fixture_${name} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
    add_dependencies(test_load_plugins load_hook_fixture_${name})
endforeach()
target_compile_definitions(load_hook_fixture_unselected PRIVATE LOAD_FAIL_FACTORY)

add_test(NAME test_load_plugins COMMAND test_load_plugins)

add_executable(test_state_persistence test_persistence.cpp)
target_link_libraries(test_state_persistence PRIVATE load_lib Boost::unit_test_framework)
add_test(NAME test_state_persistence COMMAND test_state_persistence)

# Export the test-only fsync interposer to exercise failures in the shared utility.
add_executable(test_persistence_publication test_persistence_publication.cpp)
target_link_libraries(test_persistence_publication PRIVATE
    load_lib fileio_lib Boost::unit_test_framework)
set_target_properties(test_persistence_publication PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_persistence_publication COMMAND test_persistence_publication)
