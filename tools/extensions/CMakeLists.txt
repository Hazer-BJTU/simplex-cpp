add_library(tools_extensions SHARED src/plugin.cpp)
target_include_directories(tools_extensions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_extensions PUBLIC tools_intrinsic)
target_link_libraries(tools_extensions PRIVATE yamlconfig_iface logging_lib)
set_target_properties(tools_extensions PROPERTIES OUTPUT_NAME extension_tools)

# Copy editable YAML into the build layout and install it in the identical layout.
function(simplex_install_tool_extension_schema name source_directory)
    if(NOT IS_ABSOLUTE "${source_directory}")
        message(FATAL_ERROR "Extension schema paths must be absolute")
    endif()
    if(NOT name MATCHES "^[A-Za-z0-9_-]+$")
        message(FATAL_ERROR "Invalid tool extension name: ${name}")
    endif()
    if(NOT EXISTS "${source_directory}/config.yaml")
        message(FATAL_ERROR "Missing tool extension config: ${source_directory}")
    endif()
    file(GLOB_RECURSE schema_files CONFIGURE_DEPENDS
        RELATIVE "${source_directory}" "${source_directory}/*")
    foreach(schema_file IN LISTS schema_files)
        configure_file("${source_directory}/${schema_file}"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/schemas/tools/extensions/${name}/${schema_file}"
            COPYONLY)
    endforeach()
    install(DIRECTORY "${source_directory}/" DESTINATION
        "bin/schemas/tools/extensions/${name}")
endfunction()

add_subdirectory(stubs/noop)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
