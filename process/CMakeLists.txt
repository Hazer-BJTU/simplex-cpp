# process: the child-process management module (spawn / feed stdin / collect
# output / terminate, on Boost.Process v2 + asio). Header-only for now — the
# exception contract below is the first resident; the manager framework lands
# next and will add Boost.Process + asio_iface to this target's dependencies
# (and the process component to the top-level find_package).
add_library(process_iface INTERFACE)
target_include_directories(process_iface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(process_iface INTERFACE
    simplex_thirdparty_iface
    Boost::headers)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
