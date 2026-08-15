# Deterministic unit tests use loopback sockets only and require no Internet
# access. Boost.Test supplies the executable's test runner.
add_executable(test_https_stream test_https_stream.cpp)

target_compile_options(test_https_stream PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

target_link_libraries(test_https_stream PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)

add_test(NAME test_https_stream COMMAND test_https_stream)

add_executable(test_http_request_exception test_http_request_exception.cpp)
target_link_libraries(test_http_request_exception PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_http_request_exception COMMAND test_http_request_exception)

add_executable(test_sse_response_handler test_sse_response_handler.cpp)
target_link_libraries(test_sse_response_handler PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_sse_response_handler COMMAND test_sse_response_handler)

# Offline tests for the model request interpreter interface: lenient endpoint
# resolution plus the contract's implementability through a stub.
add_executable(test_model_request test_model_request.cpp)
target_link_libraries(test_model_request PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_model_request COMMAND test_model_request)

# Manual end-to-end HTTPS check. It is intentionally not registered with CTest
# so ordinary test runs remain deterministic and work offline.
add_executable(https_connectivity_check https_connectivity_check.cpp)
target_link_libraries(https_connectivity_check PRIVATE endpoint_lib)

# End-to-end check for sse_request against a local TLS SSE server. Like the
# connectivity check it is offline (loopback + self-signed cert) but heavier, so
# it is kept out of CTest.
add_executable(sse_request_e2e sse_request_e2e.cpp)
target_link_libraries(sse_request_e2e PRIVATE endpoint_lib)
