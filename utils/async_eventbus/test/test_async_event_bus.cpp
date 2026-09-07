/**
 * @file test_async_event_bus.cpp
 * @brief Unit tests for eventbus/async_event_bus.hpp (coroutine-fold bus).
 *
 * The serial-fold contract matrix: single-handler fold, multi-handler
 * registration order, serial execution across REAL coroutine suspensions (the
 * async-specific regression — a stuck/broken ordering here is the signal),
 * disconnect/scoped disconnect, no-subscriber returns input, cross-type
 * isolation, propagate-mode exception abort, capture-mode exception recording
 * with continuation (incl. errors surviving a handler that returns a fresh
 * event), reentrant subscribe/publish from inside a handler (the deadlock
 * regression for the never-lock-across-co_await discipline + the snapshot
 * contract), concurrent publish/subscribe churn behind a barrier with explicit
 * coroutine-exception capture, concurrent-publish overlap, in-flight
 * disconnect/clear (deterministic gate, no sleeps), subscriber_count, clear,
 * and default-bus identity/independence, plus parity checks with the
 * synchronous bus (resubscribe-to-back, clear-all-types, connection state
 * after clear, input immutability, explicit-propagate equivalence, idempotent
 * disconnect).
 */

#define BOOST_TEST_MODULE AsyncEventBusTests
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <barrier>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "eventbus/async_event_bus.hpp"

namespace asio = boost::asio;

namespace {

/// The event type driven through the fold: `value` for single-field rewrites,
/// `trace` for order/list-style accumulation (a container INSIDE the event).
/// Derives from AsyncEventBase, which supplies the `errors` container that
/// capture-mode publish fills.
struct Event : eventbus::AsyncEventBase {
    int value = 0;
    std::vector<int> trace;
};

/// A second type for cross-type isolation and nested-publish cases.
struct OtherEvent : eventbus::AsyncEventBase {};

/// Build an Event with a given `value`. Using designated initializers on an
/// aggregate with a base class trips GCC's -Wmissing-field-initializers, so the
/// value-setting sites construct via this helper instead (Event{} is fine).
Event event_with(int value) {
    Event e;
    e.value = value;
    return e;
}

/// Suspend the coroutine once and resume on the next scheduler poll — the
/// explicit async hop that proves handlers really run serially, not inline.
asio::awaitable<void> yield_once() {
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
}

/// Drive a body returning awaitable<R> to completion on a fresh io_context,
/// bringing the result (or any exception) back to the caller.
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

/// Extract the message carried by an exception_ptr ("" for a non-std throw).
std::string exception_message(const std::exception_ptr& e) {
    if (!e) {
        return "<null>";
    }
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        return ex.what();
    } catch (...) {
        return "<non-std-exception>";
    }
    return "<no-exception>";  // unreachable: rethrow_exception always throws
}

} // namespace

BOOST_AUTO_TEST_SUITE(AsyncEventBusSuite)

// --- basic fold ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(single_handler_folds_result)
{
    eventbus::AsyncEventBus bus;
    bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> {
        Event out = e;
        out.value += 1;
        co_return out;
    });

    const Event result = run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(event_with(41));
    });

    BOOST_CHECK_EQUAL(result.value, 42);
}

BOOST_AUTO_TEST_CASE(handlers_fold_in_registration_order)
{
    eventbus::AsyncEventBus bus;
    bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> {
        Event out = e;
        out.trace.push_back(1);
        co_return out;
    });
    bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> {
        Event out = e;
        out.trace.push_back(2);
        co_return out;
    });
    bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> {
        Event out = e;
        out.trace.push_back(3);
        co_return out;
    });

    const Event result = run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });

    const std::vector<int> expected{1, 2, 3};
    BOOST_CHECK_EQUAL_COLLECTIONS(result.trace.begin(), result.trace.end(),
                                  expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(handlers_run_serially_across_suspensions)
{
    // The async-specific regression: each handler suspends at least once, and
    // the next handler must not start until the previous one has fully
    // completed. The timeline can only be "first.start, first.end,
    // second.start, second.end" if dispatch is serial — concurrent execution
    // would interleave the marks.
    eventbus::AsyncEventBus bus;
    std::vector<std::string> timeline;

    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        timeline.push_back("first.start");
        co_await yield_once();
        timeline.push_back("first.end");
        co_return e;
    });
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        timeline.push_back("second.start");
        co_await yield_once();
        timeline.push_back("second.end");
        co_return e;
    });

    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });

    const std::vector<std::string> expected{
        "first.start", "first.end", "second.start", "second.end"};
    BOOST_CHECK_EQUAL_COLLECTIONS(timeline.begin(), timeline.end(),
                                  expected.begin(), expected.end());
}

// --- unsubscription -----------------------------------------------------------

BOOST_AUTO_TEST_CASE(disconnect_stops_future_delivery)
{
    eventbus::AsyncEventBus bus;
    int gone_calls = 0;
    int kept_calls = 0;
    const auto gone = bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++gone_calls;
        co_return e;
    });
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++kept_calls;
        co_return e;
    });

    gone.disconnect();
    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });

    BOOST_CHECK_EQUAL(gone_calls, 0);
    BOOST_CHECK_EQUAL(kept_calls, 1);
}

BOOST_AUTO_TEST_CASE(scoped_subscription_disconnects_on_scope_exit)
{
    eventbus::AsyncEventBus bus;
    int calls = 0;
    {
        eventbus::AsyncEventBus::ScopedSubscription sub =
            bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
                ++calls;
                co_return e;
            });
        run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
            co_return co_await bus.publish(Event{});
        });
        BOOST_CHECK_EQUAL(calls, 1);
    }  // sub disconnects here

    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });
    BOOST_CHECK_EQUAL(calls, 1);
}

// --- routing ------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(publish_without_subscribers_returns_input)
{
    eventbus::AsyncEventBus bus;
    const Event result = run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(event_with(7));
    });

    BOOST_CHECK_EQUAL(result.value, 7);
    BOOST_CHECK(result.trace.empty());
    BOOST_CHECK_EQUAL(bus.subscriber_count<Event>(), 0);
}

BOOST_AUTO_TEST_CASE(distinct_event_types_do_not_cross_talk)
{
    eventbus::AsyncEventBus bus;
    int event_calls = 0;
    int other_calls = 0;
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++event_calls;
        co_return e;
    });
    bus.subscribe<OtherEvent>([&](const OtherEvent& e) -> asio::awaitable<OtherEvent> {
        ++other_calls;
        co_return e;
    });

    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });

    BOOST_CHECK_EQUAL(event_calls, 1);
    BOOST_CHECK_EQUAL(other_calls, 0);
}

// --- exception modes -----------------------------------------------------------

BOOST_AUTO_TEST_CASE(propagate_mode_aborts_on_throw)
{
    eventbus::AsyncEventBus bus;
    int after_calls = 0;
    bus.subscribe<Event>([](const Event&) -> asio::awaitable<Event> {
        co_await yield_once();  // throw AFTER a suspension — the async path
        throw std::runtime_error("boom");
    });
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++after_calls;
        co_return e;
    });

    BOOST_CHECK_THROW(
        run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
            co_return co_await bus.publish(Event{});
        }),
        std::runtime_error);
    // The fold abandoned the walk at the thrower: the next handler never ran.
    BOOST_CHECK_EQUAL(after_calls, 0);
}

BOOST_AUTO_TEST_CASE(capture_mode_records_errors_and_continues)
{
    eventbus::AsyncEventBus bus;
    bus.subscribe<Event>([](const Event&) -> asio::awaitable<Event> {
        co_await yield_once();
        throw std::runtime_error("boom");
    });
    bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> {
        Event out = e;           // copies the inherited errors onward
        out.value += 100;
        co_return out;
    });

    const Event outcome =
        run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
            co_return co_await bus.publish(event_with(5),
                                           eventbus::capture_exceptions);
        });

    BOOST_CHECK_EQUAL(outcome.errors.size(), 1);
    // The thrower left the running value at its input (5); the survivor folded
    // +100 onto it, so the final value is 105 — proof the fold continued.
    BOOST_CHECK_EQUAL(outcome.value, 105);
}

BOOST_AUTO_TEST_CASE(capture_mode_errors_survive_fresh_handler_results)
{
    // Errors are bus-owned metadata, not something handlers must carry forward:
    // a later handler that returns a freshly constructed event (NOT a copy of
    // its input) must still not lose previously captured exceptions.
    eventbus::AsyncEventBus bus;
    bus.subscribe<Event>([](const Event&) -> asio::awaitable<Event> {
        co_await yield_once();
        throw std::runtime_error("boom");
    });
    bus.subscribe<Event>([](const Event&) -> asio::awaitable<Event> {
        Event fresh;
        fresh.value = 99;
        co_return fresh;  // drops everything the bus passed in
    });

    const Event outcome =
        run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
            co_return co_await bus.publish(event_with(5),
                                           eventbus::capture_exceptions);
        });

    BOOST_CHECK_EQUAL(outcome.errors.size(), 1);
    BOOST_CHECK_EQUAL(outcome.value, 99);
}

BOOST_AUTO_TEST_CASE(capture_mode_preserves_preexisting_errors)
{
    // No subscribers, but the input already carries an error: capture mode must
    // not silently erase it (the documented "returns a copy of the input
    // unchanged" contract extends to the bus-owned errors metadata).
    eventbus::AsyncEventBus bus;
    Event input = event_with(7);
    input.errors.push_back(std::make_exception_ptr(std::runtime_error("pre-existing")));

    const Event result = run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(input, eventbus::capture_exceptions);
    });

    BOOST_REQUIRE_EQUAL(result.errors.size(), 1u);
    BOOST_CHECK_EQUAL(exception_message(result.errors[0]), "pre-existing");
}

BOOST_AUTO_TEST_CASE(capture_mode_appends_to_preexisting_errors)
{
    // Pre-existing errors are preserved AND this run's exceptions are appended
    // after them, in execution order.
    eventbus::AsyncEventBus bus;
    bus.subscribe<Event>([](const Event&) -> asio::awaitable<Event> {
        throw std::runtime_error("new");
    });

    Event input = event_with(5);
    input.errors.push_back(std::make_exception_ptr(std::runtime_error("pre-existing")));

    const Event result = run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(input, eventbus::capture_exceptions);
    });

    BOOST_REQUIRE_EQUAL(result.errors.size(), 2u);
    BOOST_CHECK_EQUAL(exception_message(result.errors[0]), "pre-existing");
    BOOST_CHECK_EQUAL(exception_message(result.errors[1]), "new");
}

// --- reentrancy ----------------------------------------------------------------

BOOST_AUTO_TEST_CASE(reentrant_publish_and_subscribe_inside_handler)
{
    // THE deadlock regression: a handler re-enters the same bus — subscribe on
    // two types plus a nested publish of the OTHER type (no infinite recursion).
    // If the registry lock were ever held across a co_await this hangs.
    //
    // Also pins the snapshot contract: a handler subscribed DURING an in-flight
    // publish must not participate in that same publish, only in later ones.
    eventbus::AsyncEventBus bus;
    int nested_calls = 0;
    bus.subscribe<OtherEvent>([&](const OtherEvent& e) -> asio::awaitable<OtherEvent> {
        ++nested_calls;
        co_return e;
    });

    int ping_calls = 0;
    int dynamic_calls = 0;
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++ping_calls;
        bus.subscribe<Event>([&](const Event& x) -> asio::awaitable<Event> {
            ++dynamic_calls;
            co_return x;
        });
        bus.subscribe<OtherEvent>([](const OtherEvent& x) -> asio::awaitable<OtherEvent> {
            co_return x;
        });
        co_await bus.publish(OtherEvent{});  // nested dispatch, other type
        co_return e;
    });

    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });

    BOOST_CHECK_EQUAL(ping_calls, 1);
    BOOST_CHECK_EQUAL(nested_calls, 1);
    // The dynamically added handler was appended mid-dispatch, so it was NOT in
    // this publish's snapshot and must not have run.
    BOOST_CHECK_EQUAL(dynamic_calls, 0);

    // A second publish picks it up from the (now-updated) registry.
    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });
    BOOST_CHECK_EQUAL(ping_calls, 2);
    BOOST_CHECK_EQUAL(dynamic_calls, 1);
}

// --- instances -----------------------------------------------------------------

BOOST_AUTO_TEST_CASE(default_bus_identity_and_independence)
{
    BOOST_CHECK_EQUAL(&eventbus::default_async_bus(), &eventbus::default_async_bus());

    eventbus::AsyncEventBus local;
    int default_calls = 0;
    int local_calls = 0;
    const auto on_default = eventbus::default_async_bus().subscribe<Event>(
        [&](const Event& e) -> asio::awaitable<Event> { ++default_calls; co_return e; });
    local.subscribe<Event>(
        [&](const Event& e) -> asio::awaitable<Event> { ++local_calls; co_return e; });

    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await local.publish(Event{});
    });
    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await eventbus::default_async_bus().publish(Event{});
    });

    BOOST_CHECK_EQUAL(default_calls, 1);
    BOOST_CHECK_EQUAL(local_calls, 1);

    // The default bus is process-global: leave it as we found it.
    on_default.disconnect();
}

// --- introspection -------------------------------------------------------------

BOOST_AUTO_TEST_CASE(subscriber_count_and_clear)
{
    eventbus::AsyncEventBus bus;
    BOOST_CHECK_EQUAL(bus.subscriber_count<Event>(), 0);

    const auto a = bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> { co_return e; });
    const auto b = bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> { co_return e; });
    BOOST_CHECK_EQUAL(bus.subscriber_count<Event>(), 2);

    a.disconnect();
    BOOST_CHECK_EQUAL(bus.subscriber_count<Event>(), 1);

    bus.clear();
    BOOST_CHECK_EQUAL(bus.subscriber_count<Event>(), 0);
    (void)b;  // disconnected by clear() above

    // The registry is clean but alive: fresh subscriptions work.
    int calls = 0;
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> { ++calls; co_return e; });
    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });
    BOOST_CHECK_EQUAL(calls, 1);
}

// --- concurrency ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(concurrent_publish_and_subscribe_churn)
{
    eventbus::AsyncEventBus bus;
    std::atomic<int> calls{0};
    std::mutex failures_mutex;
    std::vector<std::exception_ptr> failures;
    std::barrier start_gate(3);

    // One stable subscriber, registered before the threads start, so the
    // `calls > 0` assertion is deterministic regardless of how the scheduler
    // interleaves the publishers with the churn thread's first subscription.
    eventbus::AsyncEventBus::Connection stable =
        bus.subscribe<Event>([&calls](const Event& e) -> asio::awaitable<Event> {
            ++calls;
            co_return e;
        });

    std::vector<std::thread> threads;

    // Two publisher threads, each on its own io_context. A barrier start gate
    // makes the concurrent interaction deterministic rather than scheduling
    // luck; the detached publisher coroutines capture any exception explicitly
    // so a handler fault cannot be silently dropped.
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&bus, &start_gate, &failures, &failures_mutex] {
            start_gate.arrive_and_wait();
            asio::io_context io;
            asio::co_spawn(io, [&]() -> asio::awaitable<void> {
                try {
                    for (int i = 0; i < 500; ++i) {
                        co_await bus.publish(event_with(i));
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lk(failures_mutex);
                    failures.push_back(std::current_exception());
                }
            }, asio::detached);
            io.run();
        });
    }

    threads.emplace_back([&bus, &calls, &start_gate] {
        start_gate.arrive_and_wait();
        std::vector<eventbus::AsyncEventBus::Connection> live;
        for (int i = 0; i < 300; ++i) {
            live.push_back(bus.subscribe<Event>(
                [&calls](const Event& e) -> asio::awaitable<Event> {
                    ++calls;
                    co_return e;
                }));
            if (live.size() > 8) {
                live.front().disconnect();
                live.erase(live.begin());
            }
        }
        for (auto& c : live) {
            c.disconnect();
        }
    });

    for (auto& th : threads) {
        th.join();
    }

    // No handler throws, so the detached publisher coroutines ran clean.
    BOOST_CHECK(failures.empty());
    BOOST_CHECK_GT(calls.load(), 0);

    // The bus is still fully usable after the storm.
    const auto conn = bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> { co_return e; });
    const Event result = run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(event_with(9));
    });
    conn.disconnect();
    BOOST_CHECK_EQUAL(result.value, 9);
}

BOOST_AUTO_TEST_CASE(concurrent_publishes_overlap)
{
    // Independent publishes are NOT globally serialized: two publishes on two
    // threads must be able to overlap inside a handler, while each publish
    // still folds its own handlers strictly one-at-a-time.
    eventbus::AsyncEventBus bus;

    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::mutex m;
    std::condition_variable cv;
    int entered = 0;
    bool release = false;

    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        const int now = in_flight.fetch_add(1) + 1;
        int prev = max_in_flight.load();
        while (now > prev && !max_in_flight.compare_exchange_weak(prev, now)) {
        }
        {
            std::lock_guard<std::mutex> lk(m);
            ++entered;
            cv.notify_all();
        }
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return release; });
        in_flight.fetch_sub(1);
        co_return e;
    });

    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&bus] {
            asio::io_context io;
            asio::co_spawn(io, [&]() -> asio::awaitable<void> {
                co_await bus.publish(Event{});
            }, asio::detached);
            io.run();
        });
    }

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return entered == 2; });
    }
    BOOST_CHECK_EQUAL(max_in_flight.load(), 2);

    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    for (auto& th : threads) {
        th.join();
    }

    BOOST_CHECK_EQUAL(in_flight.load(), 0);
    BOOST_CHECK_EQUAL(max_in_flight.load(), 2);
}

BOOST_AUTO_TEST_CASE(disconnect_during_in_flight_publish_skips_later_handler)
{
    // The first handler suspends; while it is suspended the second is
    // disconnected; the first then resumes and completes, and the second must
    // not start (disconnect stops FUTURE dispatches, not one already running).
    eventbus::AsyncEventBus bus;

    std::mutex m;
    std::condition_variable cv;
    bool first_entered = false;
    bool release = false;
    int first_calls = 0;
    int second_calls = 0;

    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++first_calls;
        {
            std::lock_guard<std::mutex> lk(m);
            first_entered = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return release; });
        co_return e;
    });
    const auto second = bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++second_calls;
        co_return e;
    });

    std::thread io_thread([&] {
        asio::io_context io;
        asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            co_await bus.publish(Event{});
        }, asio::detached);
        io.run();
    });

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return first_entered; });
    }
    BOOST_CHECK_EQUAL(second_calls, 0);

    second.disconnect();

    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    io_thread.join();

    BOOST_CHECK_EQUAL(first_calls, 1);
    BOOST_CHECK_EQUAL(second_calls, 0);
}

BOOST_AUTO_TEST_CASE(clear_during_in_flight_publish_skips_remaining_handlers)
{
    // Same pattern as the disconnect case, but clear() disconnects everything:
    // the in-flight first handler completes, the not-yet-started second is
    // skipped, and the registry is left empty.
    eventbus::AsyncEventBus bus;

    std::mutex m;
    std::condition_variable cv;
    bool first_entered = false;
    bool release = false;
    int first_calls = 0;
    int second_calls = 0;

    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++first_calls;
        {
            std::lock_guard<std::mutex> lk(m);
            first_entered = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return release; });
        co_return e;
    });
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++second_calls;
        co_return e;
    });

    std::thread io_thread([&] {
        asio::io_context io;
        asio::co_spawn(io, [&]() -> asio::awaitable<void> {
            co_await bus.publish(Event{});
        }, asio::detached);
        io.run();
    });

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return first_entered; });
    }
    BOOST_CHECK_EQUAL(second_calls, 0);

    bus.clear();

    {
        std::lock_guard<std::mutex> lk(m);
        release = true;
    }
    cv.notify_all();
    io_thread.join();

    BOOST_CHECK_EQUAL(first_calls, 1);
    BOOST_CHECK_EQUAL(second_calls, 0);
    BOOST_CHECK_EQUAL(bus.subscriber_count<Event>(), 0);
}

// --- parity with the synchronous bus ------------------------------------------

BOOST_AUTO_TEST_CASE(disconnect_and_resubscribe_goes_to_back)
{
    eventbus::AsyncEventBus bus;
    std::vector<int> order;
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        order.push_back(1);
        co_return e;
    });
    const auto middle = bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        order.push_back(2);
        co_return e;
    });
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        order.push_back(3);
        co_return e;
    });

    std::vector<int> expected{1, 2, 3};
    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());

    middle.disconnect();
    order.clear();
    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });
    expected = {1, 3};
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());

    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        order.push_back(4);
        co_return e;
    });
    order.clear();
    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });
    expected = {1, 3, 4};
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
}

BOOST_AUTO_TEST_CASE(clear_disconnects_all_types)
{
    eventbus::AsyncEventBus bus;
    int event_calls = 0;
    int other_calls = 0;
    bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> { ++event_calls; co_return e; });
    bus.subscribe<OtherEvent>([&](const OtherEvent& e) -> asio::awaitable<OtherEvent> {
        ++other_calls;
        co_return e;
    });

    bus.clear();
    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });
    run<OtherEvent>([&](asio::io_context&) -> asio::awaitable<OtherEvent> {
        co_return co_await bus.publish(OtherEvent{});
    });

    BOOST_CHECK_EQUAL(event_calls, 0);
    BOOST_CHECK_EQUAL(other_calls, 0);
}

BOOST_AUTO_TEST_CASE(connection_reports_disconnected_after_clear)
{
    eventbus::AsyncEventBus bus;
    const auto conn = bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> { co_return e; });
    BOOST_CHECK(conn.connected());

    bus.clear();
    BOOST_CHECK(!conn.connected());
}

BOOST_AUTO_TEST_CASE(publish_does_not_mutate_input)
{
    eventbus::AsyncEventBus bus;
    bus.subscribe<Event>([](const Event& e) -> asio::awaitable<Event> {
        Event out = e;
        out.value = 99;
        co_return out;
    });

    Event input = event_with(5);
    const Event result = run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(input);
    });

    BOOST_CHECK_EQUAL(input.value, 5);
    BOOST_CHECK_EQUAL(result.value, 99);
}

BOOST_AUTO_TEST_CASE(explicit_propagate_matches_default)
{
    eventbus::AsyncEventBus bus;
    bus.subscribe<Event>([](const Event&) -> asio::awaitable<Event> {
        throw std::runtime_error("boom");
    });

    BOOST_CHECK_THROW(
        run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
            co_return co_await bus.publish(Event{});
        }),
        std::runtime_error);
    BOOST_CHECK_THROW(
        run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
            co_return co_await bus.publish(Event{}, eventbus::propagate_exceptions);
        }),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(disconnect_is_idempotent)
{
    eventbus::AsyncEventBus bus;
    int calls = 0;
    const auto conn = bus.subscribe<Event>([&](const Event& e) -> asio::awaitable<Event> {
        ++calls;
        co_return e;
    });

    conn.disconnect();
    conn.disconnect();
    BOOST_CHECK(!conn.connected());

    run<Event>([&](asio::io_context&) -> asio::awaitable<Event> {
        co_return co_await bus.publish(Event{});
    });
    BOOST_CHECK_EQUAL(calls, 0);
}

BOOST_AUTO_TEST_SUITE_END()
