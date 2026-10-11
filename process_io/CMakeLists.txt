add_library(process_io_lib SHARED src/capture.cpp src/anonymous_pipe.cpp src/posix_pty.cpp)
target_include_directories(process_io_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(process_io_lib PUBLIC asio_iface subprocess_lib)
set_target_properties(process_io_lib PROPERTIES OUTPUT_NAME simplex_process_io)
target_compile_options(process_io_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
