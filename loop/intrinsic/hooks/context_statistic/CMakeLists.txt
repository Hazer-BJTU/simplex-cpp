add_library(loop_intrinsic_context_statistic SHARED src/hook.cpp)
target_include_directories(loop_intrinsic_context_statistic PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(loop_intrinsic_context_statistic PUBLIC loop_intrinsic)
target_compile_options(loop_intrinsic_context_statistic PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
set_target_properties(loop_intrinsic_context_statistic PROPERTIES
    OUTPUT_NAME intrinsic_loop_context_statistic)

simplex_install_intrinsic_loop_hook_config(
    context_statistic "${CMAKE_CURRENT_SOURCE_DIR}/schemas/config.yaml")

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
