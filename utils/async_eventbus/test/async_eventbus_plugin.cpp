/**
 * @file async_eventbus_plugin.cpp
 * @brief A dlopened plugin exercising the process-wide default_async_bus().
 *
 * Builds into libasync_eventbus_plugin.so. It publishes and subscribes through
 * its own call to eventbus::default_async_bus(), whose definition lives in the
 * shared async_eventbus_lib — so this .so and the host executable bind the SAME
 * bus instance by SONAME. The exported C functions let the host drive both
 * directions of the cross-boundary routing.
 */

#include "cross_event.hpp"

#include <boost/asio.hpp>

#include <atomic>

namespace asio = boost::asio;

namespace {

/// Number of times the plugin's own subscriber has been invoked.
std::atomic<int> g_plugin_calls{0};

} // namespace

extern "C" {

/// Publish a CrossEvent through the process-wide bus, driven on a local
/// io_context. Any host-side subscriber runs inside this call (the fold is
/// driven to completion synchronously on the caller's thread).
void ab_plugin_publish(int value) {
    asio::io_context io;
    asio::co_spawn(io, [value]() -> asio::awaitable<void> {
        async_eventbus_test::CrossEvent ev;
        ev.value = value;
        co_await eventbus::default_async_bus().publish(ev);
    }, asio::detached);
    io.run();
}

/// Subscribe a handler on the process-wide bus; the handler increments a
/// plugin-local counter so the host can observe the reverse direction.
void ab_plugin_subscribe() {
    static eventbus::AsyncEventBus::ScopedSubscription sub =
        eventbus::default_async_bus().subscribe<async_eventbus_test::CrossEvent>(
            [](const async_eventbus_test::CrossEvent& e)
                -> asio::awaitable<async_eventbus_test::CrossEvent> {
                ++g_plugin_calls;
                co_return e;
            });
    (void)sub;  // keep the subscription alive for the process lifetime
}

/// Number of times the plugin's subscriber has run (after ab_plugin_subscribe).
int ab_plugin_calls() {
    return g_plugin_calls.load();
}

} // extern "C"
