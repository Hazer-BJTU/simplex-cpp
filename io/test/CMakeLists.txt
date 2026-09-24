add_executable(test_io_client test_client.cpp)
target_include_directories(test_io_client PRIVATE
    "${PROJECT_SOURCE_DIR}/intercom/test")
target_link_libraries(test_io_client PRIVATE
    io_lib Boost::unit_test_framework)
target_compile_options(test_io_client PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
add_test(NAME test_io_client COMMAND test_io_client)
