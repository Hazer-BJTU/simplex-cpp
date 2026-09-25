# Application-independent synchronous file operations and private POSIX RAII.
add_library(fileio_lib SHARED src/atomic_write.cpp src/session_lock.cpp src/read_prefix.cpp)
target_include_directories(fileio_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(fileio_lib PROPERTIES OUTPUT_NAME simplex_fileio)
target_compile_options(fileio_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
