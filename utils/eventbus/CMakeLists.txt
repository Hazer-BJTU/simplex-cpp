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
# The EventBus class is header-only; the default_bus() singleton lives in
# `eventbus_lib` (SHARED) so executables AND dlopened plugins bind to one
# bus instance per process (loader-uniqueness by SONAME — an inline
# singleton would fork into a private copy inside every plugin and events
# would vanish at the boundary). `eventbus_iface` remains the consumption
# target: it carries the include dir and links the shared library, so
# consumers keep linking exactly as before. Depends on Boost headers only
# — the single top-level find_package(Boost)'s Boost::headers — and
# deliberately NOT on logging_lib, so slot exceptions propagate to the
# publisher untouched.
# =============================================================================
add_library(eventbus_lib SHARED src/default_bus.cpp)
target_include_directories(eventbus_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(eventbus_lib PROPERTIES OUTPUT_NAME eventbus)
target_compile_options(eventbus_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)

add_library(eventbus_iface INTERFACE)
target_include_directories(eventbus_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(eventbus_iface INTERFACE Boost::headers eventbus_lib)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
