add_library(process_lib SHARED
    src/process_handle.cpp
)
target_include_directories(process_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(process_lib PUBLIC
    boost_process_iface 
    dataclass_iface 
    logging_lib
    asio_iface)
target_compile_options(process_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
set_target_properties(process_lib PROPERTIES OUTPUT_NAME subprocess)
if(LTO_SUPPORT)
    set_target_properties(process_lib PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
