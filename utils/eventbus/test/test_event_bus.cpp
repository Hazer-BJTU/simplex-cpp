/**
 * @file test_event_bus.cpp
 * @brief Unit tests for eventbus/event_bus.hpp (signals2-based bus).
 *
 * The dispatch contract matrix: roundtrip, multi-subscriber fan-out,
 * registration order (incl. disconnect + re-subscribe going to the back),
 * connection/scoped disconnect, no-subscriber no-op, cross-type isolation,
 * exception propagation aborting the remaining slots, default-bus identity
 * and independence, reentrant subscribe/publish from inside a slot (the
 * deadlock regression for the never-lock-around-user-code discipline),
 * concurrent publish/subscribe churn, subscriber_count, and clear().
 *
 * The reentrancy and churn cases fail by HANGING, not by assertion, if the
 * registry lock discipline regresses — a stuck test_event_bus run is the
 * signal, not a green one.
 */

#define BOOST_TEST_MODULE EventBusTests
#include <boost/test/unit_test.hpp>

#include "eventbus/event_bus.hpp"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/// The two event types used throughout: distinct payloads so cross-talk
/// and roundtrip checks can see what actually arrived.
struct PingEvent {
    int seq = 0;
};

struct PongEvent {
    std::string tag;
};

/// A third type only some cases subscribe to; the never-subscribed cases
/// use it (and a fully-disconnected Ping) instead of inventing more.
struct IdleEvent {};

} // namespace

BOOST_AUTO_TEST_SUITE(EventBusSuite)

// --- basic dispatch ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(subscribe_publish_roundtrip)
{
    eventbus::EventBus bus;
    int seen_seq = -1;
    bus.subscribe<PingEvent>([&](const PingEvent& e) { seen_seq = e.seq; });

    bus.publish(PingEvent{.seq = 42});

    BOOST_CHECK_EQUAL(seen_seq, 42);
}

BOOST_AUTO_TEST_CASE(multiple_subscribers_all_called)
{
    eventbus::EventBus bus;
    int first = 0;
    int second = 0;
    int third = 0;
    bus.subscribe<PingEvent>([&](const PingEvent& e) { first = e.seq; });
    bus.subscribe<PingEvent>([&](const PingEvent& e) { second = e.seq; });
    bus.subscribe<PingEvent>([&](const PingEvent& e) { third = e.seq; });

    bus.publish(PingEvent{.seq = 7});

    BOOST_CHECK_EQUAL(first, 7);
    BOOST_CHECK_EQUAL(second, 7);
    BOOST_CHECK_EQUAL(third, 7);
}

BOOST_AUTO_TEST_CASE(slots_run_in_registration_order)
{
    eventbus::EventBus bus;
    std::vector<int> order;
    bus.subscribe<PingEvent>([&](const PingEvent&) { order.push_back(1); });
    const auto middle = bus.subscribe<PingEvent>([&](const PingEvent&) { order.push_back(2); });
    bus.subscribe<PingEvent>([&](const PingEvent&) { order.push_back(3); });

    std::vector<int> expected{1, 2, 3};
    bus.publish(PingEvent{});
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());

    // Disconnect the middle slot: the survivors keep their relative order.
    middle.disconnect();
    order.clear();
    bus.publish(PingEvent{});
    expected = {1, 3};
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());

    // Re-subscribing goes to the BACK, not back into its old position.
    bus.subscribe<PingEvent>([&](const PingEvent&) { order.push_back(4); });
    order.clear();
    bus.publish(PingEvent{});
    expected = {1, 3, 4};
    BOOST_CHECK_EQUAL_COLLECTIONS(order.begin(), order.end(), expected.begin(), expected.end());
}

// --- unsubscription ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(disconnect_via_connection_stops_delivery)
{
    eventbus::EventBus bus;
    int gone_calls = 0;
    int kept_calls = 0;
    const auto gone = bus.subscribe<PingEvent>([&](const PingEvent&) { ++gone_calls; });
    bus.subscribe<PingEvent>([&](const PingEvent&) { ++kept_calls; });

    gone.disconnect();
    bus.publish(PingEvent{});

    BOOST_CHECK_EQUAL(gone_calls, 0);
    BOOST_CHECK_EQUAL(kept_calls, 1);
}

BOOST_AUTO_TEST_CASE(scoped_subscription_disconnects_on_scope_exit)
{
    eventbus::EventBus bus;
    int calls = 0;
    {
        eventbus::EventBus::ScopedSubscription sub =
            bus.subscribe<PingEvent>([&](const PingEvent&) { ++calls; });
        bus.publish(PingEvent{});
        BOOST_CHECK_EQUAL(calls, 1);
    }  // sub disconnects here

    bus.publish(PingEvent{});
    BOOST_CHECK_EQUAL(calls, 1);
}

// --- routing -----------------------------------------------------------------

BOOST_AUTO_TEST_CASE(publish_without_subscribers_is_noop)
{
    eventbus::EventBus bus;
    // A type nobody ever subscribed to...
    bus.publish(IdleEvent{});
    // ...and a type whose subscribers all disconnected.
    const auto conn = bus.subscribe<PingEvent>([](const PingEvent&) {});
    conn.disconnect();
    bus.publish(PingEvent{});
    BOOST_CHECK_EQUAL(bus.subscriber_count<PingEvent>(), 0);
}

BOOST_AUTO_TEST_CASE(distinct_event_types_do_not_cross_talk)
{
    eventbus::EventBus bus;
    int ping_calls = 0;
    int pong_calls = 0;
    bus.subscribe<PingEvent>([&](const PingEvent&) { ++ping_calls; });
    bus.subscribe<PongEvent>([&](const PongEvent& e) { ++pong_calls; (void)e; });

    bus.publish(PingEvent{.seq = 1});
    bus.publish(PongEvent{.tag = "x"});

    BOOST_CHECK_EQUAL(ping_calls, 1);
    BOOST_CHECK_EQUAL(pong_calls, 1);
}

// --- exceptions ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(slot_exception_propagates_and_stops_dispatch)
{
    eventbus::EventBus bus;
    int after_throw_calls = 0;
    bus.subscribe<PingEvent>([](const PingEvent&) { throw std::runtime_error("boom"); });
    bus.subscribe<PingEvent>([&](const PingEvent&) { ++after_throw_calls; });

    BOOST_CHECK_THROW(bus.publish(PingEvent{}), std::runtime_error);
    // The combiner walks in order and abandons the walk on the throw:
    // slots after the thrower were not invoked.
    BOOST_CHECK_EQUAL(after_throw_calls, 0);
}

// --- instances ----------------------------------------------------------------

BOOST_AUTO_TEST_CASE(default_bus_identity_and_independence)
{
    // Same instance every call...
    BOOST_CHECK_EQUAL(&eventbus::default_bus(), &eventbus::default_bus());

    // ...and fully independent from any local bus.
    eventbus::EventBus local;
    int default_calls = 0;
    int local_calls = 0;
    const auto on_default =
        eventbus::default_bus().subscribe<PingEvent>([&](const PingEvent&) { ++default_calls; });
    local.subscribe<PingEvent>([&](const PingEvent&) { ++local_calls; });

    local.publish(PingEvent{});
    eventbus::default_bus().publish(PingEvent{});

    BOOST_CHECK_EQUAL(default_calls, 1);
    BOOST_CHECK_EQUAL(local_calls, 1);

    // The default bus is process-global: leave it as we found it.
    on_default.disconnect();
}

// --- reentrancy ---------------------------------------------------------------

BOOST_AUTO_TEST_CASE(reentrant_subscribe_and_publish_inside_slot)
{
    // THE deadlock regression: every path re-enters the same bus from
    // inside a running slot. If the registry lock were ever held across
    // slot invocation this test hangs (it cannot fail by assertion).
    eventbus::EventBus bus;
    int pong_calls = 0;
    bus.subscribe<PongEvent>([&](const PongEvent&) { ++pong_calls; });

    int ping_calls = 0;
    bus.subscribe<PingEvent>([&](const PingEvent&) {
        ++ping_calls;
        bus.subscribe<PingEvent>([](const PingEvent&) {});  // same-type lookup
        bus.subscribe<PongEvent>([](const PongEvent&) {});  // other-type insert
        bus.publish(PongEvent{.tag = "nested"});            // nested dispatch
    });

    bus.publish(PingEvent{.seq = 1});

    BOOST_CHECK_EQUAL(ping_calls, 1);
    BOOST_CHECK_EQUAL(pong_calls, 1);
}

BOOST_AUTO_TEST_CASE(concurrent_publish_and_subscribe_churn)
{
    eventbus::EventBus bus;
    std::atomic<int> calls{0};

    // Churn threads keep a bounded set of live connections, disconnecting
    // the oldest once the budget is reached, so subscribe and disconnect
    // race the publishers throughout.
    std::vector<eventbus::EventBus::Connection> live;
    std::mutex live_mutex;
    const auto churn = [&] {
        for (int i = 0; i < 200; ++i) {
            auto conn = bus.subscribe<PingEvent>([&](const PingEvent&) { ++calls; });
            {
                std::lock_guard<std::mutex> lock(live_mutex);
                live.push_back(std::move(conn));
                if (live.size() > 8) {
                    auto oldest = std::move(live.front());
                    live.erase(live.begin());
                    oldest.disconnect();
                }
            }
        }
    };

    const auto publish = [&] {
        for (int i = 0; i < 1000; ++i) {
            bus.publish(PingEvent{.seq = i});
        }
    };

    std::thread churners[2]{std::thread(churn), std::thread(churn)};
    std::thread publishers[2]{std::thread(publish), std::thread(publish)};
    for (auto& t : churners) {
        t.join();
    }
    for (auto& t : publishers) {
        t.join();
    }

    BOOST_CHECK_GT(calls.load(), 0);
    // The bus is still fully usable after the storm.
    const auto conn = bus.subscribe<PingEvent>([&](const PingEvent& e) { calls = e.seq; });
    bus.publish(PingEvent{.seq = 5});
    conn.disconnect();
    BOOST_CHECK_EQUAL(calls.load(), 5);
}

// --- introspection ------------------------------------------------------------

BOOST_AUTO_TEST_CASE(subscriber_count_tracks_subscriptions)
{
    eventbus::EventBus bus;
    BOOST_CHECK_EQUAL(bus.subscriber_count<IdleEvent>(), 0);

    const auto first = bus.subscribe<PingEvent>([](const PingEvent&) {});
    const auto second = bus.subscribe<PingEvent>([](const PingEvent&) {});
    BOOST_CHECK_EQUAL(bus.subscriber_count<PingEvent>(), 2);

    first.disconnect();
    BOOST_CHECK_EQUAL(bus.subscriber_count<PingEvent>(), 1);

    (void)second;  // disconnected by clear() below
    bus.clear();
    BOOST_CHECK_EQUAL(bus.subscriber_count<PingEvent>(), 0);
}

BOOST_AUTO_TEST_CASE(clear_disconnects_all_types)
{
    eventbus::EventBus bus;
    int ping_calls = 0;
    int pong_calls = 0;
    bus.subscribe<PingEvent>([&](const PingEvent&) { ++ping_calls; });
    bus.subscribe<PongEvent>([&](const PongEvent&) { ++pong_calls; });

    bus.clear();
    bus.publish(PingEvent{});
    bus.publish(PongEvent{});

    BOOST_CHECK_EQUAL(ping_calls, 0);
    BOOST_CHECK_EQUAL(pong_calls, 0);

    // The registry is clean but alive: fresh subscriptions work.
    bus.subscribe<PingEvent>([&](const PingEvent&) { ++ping_calls; });
    bus.publish(PingEvent{});
    BOOST_CHECK_EQUAL(ping_calls, 1);
}

BOOST_AUTO_TEST_SUITE_END()
