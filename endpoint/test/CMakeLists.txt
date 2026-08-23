# Deterministic unit tests use loopback sockets only and require no Internet
# access. Boost.Test supplies the executable's test runner.
#
# test_https_stream and test_http_stream deliberately keep exercising the
# DEPRECATED flavour factories (that is what they pin), so their deprecation
# warnings are silenced below.

# The runtime-flavour facade + the scheme-selecting factory behind it.
add_executable(test_connection_stream test_connection_stream.cpp)
target_compile_options(test_connection_stream PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_link_libraries(test_connection_stream PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_connection_stream COMMAND test_connection_stream)

add_executable(test_https_stream test_https_stream.cpp)
target_compile_options(test_https_stream PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
    -Wno-deprecated-declarations
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

# Plain-HTTP flavour of the deprecated connection factory: same loopback-only
# shape as test_https_stream, minus the TLS handshake.
add_executable(test_http_stream test_http_stream.cpp)
target_compile_options(test_http_stream PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
    -Wno-deprecated-declarations
)
target_link_libraries(test_http_stream PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_http_stream COMMAND test_http_stream)

# sse_request over a plain stream against a one-shot loopback SSE server: the
# deterministic (offline, socket-level) half of the sse_request coverage. The
# TLS flavour of the same flow stays in the manual sse_request_e2e executable.
add_executable(test_sse_request test_sse_request.cpp)
target_compile_options(test_sse_request PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_link_libraries(test_sse_request PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_sse_request COMMAND test_sse_request)

# http_request (the bounded single-body counterpart of sse_request): direct
# and AsyncResponseHandler overloads against the same loopback server shape.
add_executable(test_http_request test_http_request.cpp)
target_compile_options(test_http_request PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_link_libraries(test_http_request PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_http_request COMMAND test_http_request)

# complete_once — the whole-exchange convenience: the Connect-stage wrapping
# of connect failures (transport error code kept), the log-only exception
# policy of the spawned producer task, and the inline consumer drain. Same
# one-shot loopback shape; the refused-connect case needs no server at all.
add_executable(test_complete_once test_complete_once.cpp)
target_compile_options(test_complete_once PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_link_libraries(test_complete_once PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_complete_once COMMAND test_complete_once)

# Offline tests for the model request interpreter interface: lenient endpoint
# resolution plus the contract's implementability through a stub.
add_executable(test_model_request test_model_request.cpp)
target_link_libraries(test_model_request PRIVATE
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_model_request COMMAND test_model_request)

# Offline tests for the Responses-API compatibility layer: the request
# interpreter's body/header mapping, the stream handler's event decode, and
# the reader's consume loop — accumulation/assembly, statuses and the
# sync/async hooks (io_context, no sockets).
add_executable(test_responses_interpreter test_responses_interpreter.cpp)
target_link_libraries(test_responses_interpreter PRIVATE
    responses_api_lib
    Boost::unit_test_framework
)
add_test(NAME test_responses_interpreter COMMAND test_responses_interpreter)

add_executable(test_responses_stream test_responses_stream.cpp)
target_link_libraries(test_responses_stream PRIVATE
    responses_api_lib
    Boost::unit_test_framework
)
add_test(NAME test_responses_stream COMMAND test_responses_stream)

# The reader tests also drive the pump() producer wrapper against the
# one-shot loopback server (plain HTTP, offline).
add_executable(test_responses_reader test_responses_reader.cpp)
target_link_libraries(test_responses_reader PRIVATE
    responses_api_lib
    endpoint_lib
    Boost::unit_test_framework
)
add_test(NAME test_responses_reader COMMAND test_responses_reader)

# Manual end-to-end HTTPS check. It is intentionally not registered with CTest
# so ordinary test runs remain deterministic and work offline.
add_executable(https_connectivity_check https_connectivity_check.cpp)
target_link_libraries(https_connectivity_check PRIVATE endpoint_lib)

# End-to-end check for sse_request against a local TLS SSE server. Like the
# connectivity check it is offline (loopback + self-signed cert) but heavier, so
# it is kept out of CTest.
add_executable(sse_request_e2e sse_request_e2e.cpp)
target_link_libraries(sse_request_e2e PRIVATE endpoint_lib)
