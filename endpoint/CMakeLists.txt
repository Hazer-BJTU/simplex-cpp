# endpoint: provider-neutral HTTP/HTTPS and SSE transport only. Protocol
# adapters belong to their model package (the Responses adapter is in llm).
add_library(endpoint_lib INTERFACE)

target_include_directories(endpoint_lib INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(endpoint_lib INTERFACE
    Boost::headers
    openssl_iface
    logging_lib
    simplex_thirdparty_iface
    # The model request interpreter interface consumes the data contract
    # (AgentInputState) and the provider endpoint configuration records.
    dataclass_iface
)

# Keep tests optional under the project-wide BUILD_TESTS switch.
if(BUILD_TESTS)
    add_subdirectory(test)
endif()

# Interactive examples against live HTTPS services. Built unconditionally (a
# single small translation unit); not registered with CTest.
add_subdirectory(example)
