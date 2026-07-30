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

# Manual end-to-end HTTPS check. It is intentionally not registered with CTest
# so ordinary test runs remain deterministic and work offline.
add_executable(https_connectivity_check https_connectivity_check.cpp)
target_link_libraries(https_connectivity_check PRIVATE endpoint_lib)
