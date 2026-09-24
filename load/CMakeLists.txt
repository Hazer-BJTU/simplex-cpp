# Ship editable startup configuration without embedding it in a binary.
# COPYONLY preserves environment references such as ${DEEPSEEK_API_KEY}.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config.example.yaml"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/config.example.yaml"
    COPYONLY)

# Install only the example, never an operator's active config.yaml.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config.example.yaml"
    DESTINATION bin)

# Startup discovery and configured extension construction.
add_library(load_lib SHARED src/plugins.cpp src/persistence.cpp)
target_include_directories(load_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(load_lib PUBLIC llm_iface tools_extensions loop_extensions)
target_link_libraries(load_lib PRIVATE yamlconfig_iface fileio_lib)
set_target_properties(load_lib PROPERTIES OUTPUT_NAME simplex_load)
target_compile_options(load_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
