# Unit tests for eventbus/async_event_bus.hpp.
#
# test_async_event_bus — the serial-fold contract matrix: single-handler fold,
# multi-handler registration order, serial execution across real suspensions,
# disconnect/scoped disconnect, no-subscriber returns input, cross-type
# isolation, propagate-mode exception abort, capture-mode exception recording
# with continuation (incl. errors surviving a handler that returns a fresh
# event), reentrant subscribe/publish (deadlock regression + snapshot contract),
# concurrent publish/subscribe churn behind a barrier with explicit coroutine-
# exception capture, concurrent-publish overlap, in-flight disconnect/clear via
# a deterministic gate, subscriber_count, clear, default-bus identity, and
# parity checks with the synchronous bus (resubscribe-to-back, clear-all-types,
# connection state after clear, input immutability, explicit-propagate
# equivalence, idempotent disconnect).
#
# test_async_eventbus_dso — the real cross-DSO guarantee: a dlopened plugin .so
# (libasync_eventbus_plugin.so) and this host both bind the process-wide
# default_async_bus() singleton by SONAME, and events route across the boundary
# in both directions. The host is ENABLE_EXPORTS (-rdynamic) so the plugin binds
# the host's typeinfo for the shared event type (type identity, not the
# libstdc++ name-comparison fallback), mirroring the deepseek plugin test.
#
# async_eventbus_iface carries asio_iface (Boost headers + BOOST_ASIO_SEPARATE
# COMPILATION + libasio.so) transitively. std::thread (the churn/in-flight cases)
# needs pthread: -pthread is a plain compile option, preserving the
# modules-never-find_package rule.

set(_ab_test_opts
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)

# --- in-process unit tests ----------------------------------------------------
add_executable(test_async_event_bus test_async_event_bus.cpp)
target_compile_options(test_async_event_bus PRIVATE -pthread ${_ab_test_opts})
target_link_libraries(test_async_event_bus PRIVATE
    async_eventbus_iface
    Boost::unit_test_framework
)
add_test(NAME test_async_event_bus COMMAND test_async_event_bus)

# --- cross-DSO plugin + host --------------------------------------------------
# A separate domain publishing/subscribing through its own call to
# default_async_bus(), proving the SHARED-lib singleton and the event-type
# routing both hold across the dlopen boundary.
add_library(async_eventbus_plugin MODULE async_eventbus_plugin.cpp)
target_include_directories(async_eventbus_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(async_eventbus_plugin PRIVATE ${_ab_test_opts})
target_link_libraries(async_eventbus_plugin PRIVATE async_eventbus_iface)
set_target_properties(async_eventbus_plugin PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/async_eventbus_test_modules")

add_executable(test_async_eventbus_dso test_async_eventbus_dso.cpp)
target_include_directories(test_async_eventbus_dso PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(test_async_eventbus_dso PRIVATE -pthread ${_ab_test_opts})
target_link_libraries(test_async_eventbus_dso PRIVATE
    async_eventbus_iface
    Boost::unit_test_framework
    ${CMAKE_DL_LIBS}
)
target_compile_definitions(test_async_eventbus_dso PRIVATE
    "ASYNC_EVENTBUS_PLUGIN_PATH=\"$<TARGET_FILE:async_eventbus_plugin>\"")
# ENABLE_EXPORTS (-rdynamic): the dlopened plugin binds this executable's
# authoritative typeinfo for the shared CrossEvent type, so typeid-based
# routing matches across the boundary.
set_target_properties(test_async_eventbus_dso PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(test_async_eventbus_dso async_eventbus_plugin)
add_test(NAME test_async_eventbus_dso COMMAND test_async_eventbus_dso)

# A deadlock regression in these tests manifests as a hang; give CTest a
# timeout so a stuck run fails cleanly instead of hanging the job forever.
set_tests_properties(test_async_event_bus test_async_eventbus_dso
    PROPERTIES TIMEOUT 60)
