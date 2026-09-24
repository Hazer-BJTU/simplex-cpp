# JSON message routing and signal handling over intercom's text WebSocket client.
add_library(io_lib SHARED src/client.cpp)
target_include_directories(io_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(io_lib PUBLIC intercom_iface eventbus_iface)
set_target_properties(io_lib PROPERTIES OUTPUT_NAME simplex_io)
target_compile_options(io_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
