# Shared, header-only exception types used across project modules.
add_library(exceptions_lib INTERFACE)

target_include_directories(exceptions_lib INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(exceptions_lib INTERFACE Boost::headers)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
