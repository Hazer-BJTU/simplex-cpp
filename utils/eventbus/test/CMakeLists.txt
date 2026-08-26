# Unit tests for eventbus/event_bus.hpp.
#
# test_event_bus — the dispatch contract matrix: roundtrip, multi-subscriber
# fan-out, registration order (incl. disconnect + re-subscribe going to the
# back), connection/scoped disconnect, no-subscriber no-op, cross-type
# isolation, exception propagation aborting the remaining slots, default-bus
# identity, reentrant subscribe/publish from inside a slot (deadlock
# regression), concurrent publish/subscribe churn, subscriber_count, and
# clear().
#
# eventbus_iface carries Boost::headers transitively. std::thread (the
# reentrancy and churn cases) needs pthread: -pthread is a plain compile
# option, preserving the modules-never-find_package rule.
add_executable(test_event_bus test_event_bus.cpp)
target_compile_options(test_event_bus PRIVATE
    -pthread
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_link_libraries(test_event_bus PRIVATE
    eventbus_iface
    Boost::unit_test_framework
)
add_test(NAME test_event_bus COMMAND test_event_bus)
