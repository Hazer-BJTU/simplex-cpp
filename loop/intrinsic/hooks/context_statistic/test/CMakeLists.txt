add_executable(test_loop_context_statistic test_context_statistic.cpp)
target_link_libraries(test_loop_context_statistic PRIVATE
    loop_intrinsic_context_statistic Boost::unit_test_framework)
target_compile_options(test_loop_context_statistic PRIVATE -pthread)
add_test(NAME test_loop_context_statistic COMMAND test_loop_context_statistic)

# Build the real loader and hook with no source fallback for this lookup test.
add_executable(test_loop_installed_config
    test_installed_config.cpp
    ${CMAKE_SOURCE_DIR}/loop/intrinsic/src/hook_config.cpp
    ${CMAKE_SOURCE_DIR}/loop/intrinsic/hooks/context_statistic/src/hook.cpp)
target_include_directories(test_loop_installed_config PRIVATE
    ${CMAKE_SOURCE_DIR}/loop/intrinsic/include
    ${CMAKE_SOURCE_DIR}/loop/intrinsic/hooks/context_statistic/include)
target_compile_definitions(test_loop_installed_config PRIVATE
    SIMPLEX_LOOP_HOOK_SOURCE_ROOT="/nonexistent/simplex-loop-hook-schemas")
target_link_libraries(test_loop_installed_config PRIVATE
    loop_lib yamlconfig_iface logging_lib)
add_test(NAME test_loop_installed_config
    COMMAND ${CMAKE_COMMAND}
        -Dstage=${CMAKE_CURRENT_BINARY_DIR}/staged_config
        -Dexecutable=$<TARGET_FILE:test_loop_installed_config>
        -Dsource_config=${CMAKE_CURRENT_SOURCE_DIR}/../schemas/config.yaml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_staged_config.cmake)
