# Shared YAML configuration core for built-in loop hooks. No hook is compiled
# here yet; concrete packages link this library and install their own YAML.
add_library(loop_intrinsic SHARED src/hook_config.cpp)
target_include_directories(loop_intrinsic PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(loop_intrinsic PUBLIC loop_lib)
target_link_libraries(loop_intrinsic PRIVATE yamlconfig_iface logging_lib)
target_compile_definitions(loop_intrinsic PRIVATE
    SIMPLEX_LOOP_HOOK_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/hooks")
target_compile_options(loop_intrinsic PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
set_target_properties(loop_intrinsic PROPERTIES OUTPUT_NAME intrinsic_loop_hooks)

# One call in each concrete hook package's CMakeLists. As with process tools,
# listing a YAML file in add_library() alone does not export it on install.
function(simplex_install_intrinsic_loop_hook_config hook_name source_file)
    if(NOT hook_name MATCHES "^[A-Za-z0-9_-]+$")
        message(FATAL_ERROR "Invalid intrinsic loop hook name: ${hook_name}")
    endif()
    if(NOT IS_ABSOLUTE "${source_file}")
        message(FATAL_ERROR "Intrinsic loop hook config path must be absolute: ${source_file}")
    endif()
    if(NOT EXISTS "${source_file}")
        message(FATAL_ERROR "Missing intrinsic loop hook config: ${source_file}")
    endif()
    install(FILES "${source_file}"
        DESTINATION "bin/schemas/loop/${hook_name}"
        RENAME config.yaml)
endfunction()

add_subdirectory(hooks)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
