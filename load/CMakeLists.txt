# Ship editable startup configuration without embedding it in a binary.
# COPYONLY preserves environment references such as ${DEEPSEEK_API_KEY}.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/schemas/config.example.yaml"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/config.example.yaml"
    COPYONLY)

# Install only the example, never an operator's active config.yaml.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/schemas/config.example.yaml"
    DESTINATION bin)

# JSON codec stays independent of runtime/provider loading for the manager.
add_library(load_persistence SHARED src/persistence.cpp)
target_include_directories(load_persistence PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(load_persistence PUBLIC dataclass_iface PRIVATE fileio_lib)
set_target_properties(load_persistence PROPERTIES OUTPUT_NAME simplex_load_persistence)

# Archive retention stays independent of startup discovery and plugin loading:
# the persistence backend applies it, and a state-inspection client never needs
# the plugin stack. It only depends on the standard library.
add_library(load_archives SHARED src/archives.cpp)
target_include_directories(load_archives PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(load_archives PROPERTIES OUTPUT_NAME simplex_load_archives)
target_compile_options(load_archives PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)

# Startup discovery and configured extension construction.
add_library(load_lib SHARED src/plugins.cpp src/configuration.cpp src/system_prompt.cpp)
target_include_directories(load_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(load_lib PUBLIC fileio_lib load_persistence load_archives llm_iface tools_extensions loop_extensions io_lib)
target_link_libraries(load_lib PRIVATE manager_lib yamlconfig_iface subprocess_lib)
set_target_properties(load_lib PROPERTIES OUTPUT_NAME simplex_load)
target_compile_options(load_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
