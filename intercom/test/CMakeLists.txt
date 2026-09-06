# Deterministic unit tests use loopback WebSocket servers only and require no
# Internet access. Boost.Test supplies the executable's test runner.

# connect_websocket + fetch_once + fetch (retry engine) + the is_recoverable
# verdict, against loopback ws:// servers. The refused-connect case needs no
# server at all.
add_executable(test_ws_fetch test_ws_fetch.cpp)
target_compile_options(test_ws_fetch PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_link_libraries(test_ws_fetch PRIVATE
    intercom_iface
    Boost::unit_test_framework
)
add_test(NAME test_ws_fetch COMMAND test_ws_fetch)
