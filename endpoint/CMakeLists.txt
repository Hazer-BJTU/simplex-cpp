# endpoint is currently header-only. Consumers link this interface target to
# inherit the public include path together with Boost and OpenSSL dependencies.
add_library(endpoint_lib INTERFACE)

target_include_directories(endpoint_lib INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(endpoint_lib INTERFACE
    Boost::headers
    openssl_iface
)

# Keep tests optional under the project-wide BUILD_TESTS switch.
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
