# =============================================================================
# eventbus — module (utils package)
#
# Type-dispatched synchronous publish/subscribe bus: events are plain
# copyable structs whose C++ type is the topic; bus.publish(evt) invokes
# every bus.subscribe<Evt>(slot) registration immediately on the
# publisher's thread, in registration order. Built on boost::signals2
# (slot storage, connection handles, concurrent-invocation guarantees);
# the bus itself adds the type_index registry, the registration-order slot
# groups, and the never-lock-around-user-code reentrancy discipline.
#
# Header-only (`eventbus_iface`, INTERFACE): consumers link it and include
# "eventbus/event_bus.hpp". Depends on Boost headers only — the imported
# Boost::headers target from the single top-level find_package(Boost) —
# and deliberately NOT on logging_lib, so slot exceptions propagate to the
# publisher untouched.
# =============================================================================
add_library(eventbus_iface INTERFACE)
target_include_directories(eventbus_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(eventbus_iface INTERFACE Boost::headers)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
