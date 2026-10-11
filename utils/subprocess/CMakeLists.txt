find_package(Threads REQUIRED)

# Native process and executable-location implementation stays private.
add_library(subprocess_lib SHARED src/child.cpp src/platform.cpp src/pipe_writer.cpp src/terminal.cpp src/terminal_launch.cpp)
target_include_directories(subprocess_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(subprocess_lib PUBLIC asio_iface dataclass_iface
    PRIVATE boost_process_iface boost_dll_iface Threads::Threads)
set_target_properties(subprocess_lib PROPERTIES OUTPUT_NAME simplex_subprocess_platform)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
