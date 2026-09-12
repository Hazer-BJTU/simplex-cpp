add_library(tools_iface INTERFACE)
target_include_directories(tools_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_iface INTERFACE
    extensions_iface
    dataclass_iface
    simplex_versioning
    asio_iface
)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
