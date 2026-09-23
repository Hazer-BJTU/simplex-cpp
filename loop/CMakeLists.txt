# Single active loop per harness process.
add_library(loop_lib SHARED
    src/hook_interface.cpp
    src/hook_registry.cpp
    src/loop.cpp)
target_include_directories(loop_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(loop_lib PUBLIC
    dataclass_iface
    asio_iface
    eventbus_iface
    llm_iface
    tools_iface
    logging_lib)
set_target_properties(loop_lib PROPERTIES OUTPUT_NAME agent_loop)
target_compile_options(loop_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()

add_subdirectory(example)
add_subdirectory(intrinsic)
