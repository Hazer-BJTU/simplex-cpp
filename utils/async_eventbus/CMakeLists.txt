# =============================================================================
# async_eventbus — module (utils package)
#
# Type-dispatched serial-async publish/subscribe bus: events are plain
# copyable structs whose C++ type is the topic; handlers are Boost.Asio
# coroutines (boost::asio::awaitable<Evt>(const Evt&)), and publish() FOLDS the
# event through them one at a time in registration order — each handler's
# co_return value becomes the next handler's input, and publish returns the
# final Evt. Two exception modes, selected by a token parameter:
# publish(evt) / publish(evt, propagate_exceptions) let a throwing handler
# abort the fold and propagate; publish(evt, capture_exceptions) records each
# exception in PublishOutcome::errors and continues. The registry mutex is
# never held across a co_await, so handlers may re-enter the bus freely.
#
# The AsyncEventBus class is header-only; the default_async_bus() singleton
# lives in `async_eventbus_lib` (SHARED) so executables AND dlopened plugins
# bind to one bus instance per process by SONAME (same medicine as
# eventbus_lib). `async_eventbus_iface` remains the consumption target: it
# carries the include dir and links the shared library. Unlike the signals2
# bus it depends on Boost.Asio, so it links `asio_iface` (the one-compiled-copy
# rule) — and deliberately NOT on logging_lib, so handler exceptions are the
# publisher's problem untouched.
# =============================================================================
add_library(async_eventbus_lib SHARED src/default_async_bus.cpp)
target_include_directories(async_eventbus_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(async_eventbus_lib PROPERTIES OUTPUT_NAME async_eventbus)
target_link_libraries(async_eventbus_lib PRIVATE asio_iface)
target_compile_options(async_eventbus_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)

add_library(async_eventbus_iface INTERFACE)
target_include_directories(async_eventbus_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(async_eventbus_iface INTERFACE asio_iface async_eventbus_lib)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
