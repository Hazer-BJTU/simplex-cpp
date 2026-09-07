/**
 * @file test_async_eventbus_dso.cpp
 * @brief Cross-DSO tests for the process-wide default_async_bus().
 *
 * The whole point of compiling default_async_bus() into async_eventbus_lib
 * (SHARED) is that an executable and a dlopened plugin observe ONE bus. This
 * drives that claim in both directions over a real .so:
 *
 *   - host subscribes, plugin publishes  -> the host subscriber receives the event
 *   - plugin subscribes, host publishes  -> the plugin subscriber is invoked
 *
 * The shared CrossEvent type (cross_event.hpp), the host's -rdynamic export,
 * and the SHARED singleton together make the typeid-routed bus hold across the
 * dlopen boundary. The same medicine is applied by the deepseek plugin test for
 * the synchronous bus; this is the async bus's counterpart.
 */

#define BOOST_TEST_MODULE AsyncEventBusDsoTests
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <dlfcn.h>

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "cross_event.hpp"

namespace asio = boost::asio;

#ifndef ASYNC_EVENTBUS_PLUGIN_PATH
#error ASYNC_EVENTBUS_PLUGIN_PATH must name the built plugin library
#endif

namespace {

/// Load the plugin .so once (dlopen refcounts; both cases share the handle).
/// Uses RTLD_LOCAL (the production loader's visibility semantics, NOT the more
/// permissive RTLD_GLOBAL) plus RTLD_NODELETE where supported, so a
/// symbol-resolution problem that the real loader would hit is not hidden here.
void* load_plugin() {
    int flags = RTLD_NOW | RTLD_LOCAL;
#ifdef RTLD_NODELETE
    flags |= RTLD_NODELETE;
#endif
    void* handle = dlopen(ASYNC_EVENTBUS_PLUGIN_PATH, flags);
    const char* err = dlerror();  // capture once; dlerror() clears on each call
    BOOST_REQUIRE_MESSAGE(handle,
                          "dlopen failed: " << (err ? err : "unknown"));
    return handle;
}

template <typename Fn>
Fn* load_symbol(void* handle, const char* name) {
    dlerror();  // clear any stale error
    auto* sym = reinterpret_cast<Fn*>(dlsym(handle, name));
    const char* err = dlerror();
    BOOST_REQUIRE_MESSAGE(sym,
                          "dlsym(" << name << ") failed: " << (err ? err : "unknown"));
    return sym;
}

/// Drive a body returning awaitable<R> to completion on a fresh io_context.
template <typename R>
R run(std::function<asio::awaitable<R>(asio::io_context&)> body) {
    asio::io_context io;
    std::optional<R> result;
    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            result.emplace(co_await body(io));
        } catch (...) {
            failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return std::move(*result);
}

} // namespace

BOOST_AUTO_TEST_SUITE(AsyncEventBusDsoSuite)

BOOST_AUTO_TEST_CASE(host_subscriber_receives_plugin_publish)
{
    void* handle = load_plugin();
    using PublishFn = void(int);
    auto* plugin_publish = load_symbol<PublishFn>(handle, "ab_plugin_publish");

    std::vector<int> received;
    eventbus::AsyncEventBus::ScopedSubscription sub =
        eventbus::default_async_bus().subscribe<async_eventbus_test::CrossEvent>(
            [&](const async_eventbus_test::CrossEvent& e)
                -> asio::awaitable<async_eventbus_test::CrossEvent> {
                received.push_back(e.value);
                co_return e;
            });

    plugin_publish(42);

    BOOST_REQUIRE_EQUAL(received.size(), 1u);
    BOOST_CHECK_EQUAL(received[0], 42);
}

BOOST_AUTO_TEST_CASE(plugin_subscriber_receives_host_publish)
{
    void* handle = load_plugin();
    using SubscribeFn = void();
    using CallsFn = int();
    auto* plugin_subscribe = load_symbol<SubscribeFn>(handle, "ab_plugin_subscribe");
    auto* plugin_calls = load_symbol<CallsFn>(handle, "ab_plugin_calls");

    plugin_subscribe();
    BOOST_CHECK_EQUAL(plugin_calls(), 0);

    run<async_eventbus_test::CrossEvent>(
        [](asio::io_context&) -> asio::awaitable<async_eventbus_test::CrossEvent> {
            async_eventbus_test::CrossEvent ev;
            ev.value = 7;
            co_return co_await eventbus::default_async_bus().publish(ev);
        });

    BOOST_CHECK_EQUAL(plugin_calls(), 1);
}

BOOST_AUTO_TEST_SUITE_END()
