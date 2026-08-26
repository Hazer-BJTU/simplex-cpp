#pragma once

/**
 * @file eventbus/event_bus.hpp
 * @brief Type-dispatched synchronous publish/subscribe event bus.
 *
 * Modules that want to react to each other's state changes (session
 * lifecycle, stream progress, ...) without knowing each other meet here.
 * An event is a plain copyable struct — no base class, no registration
 * ceremony; its C++ type IS the topic. publish() invokes every
 * subscribe<Evt>() slot immediately, on the publisher's thread, in
 * registration order.
 *
 * Usage:
 *   struct SessionStarted { std::string id; };
 *
 *   auto& bus = eventbus::default_bus();
 *   eventbus::EventBus::ScopedSubscription sub =
 *       bus.subscribe<SessionStarted>([&](const SessionStarted& e) {
 *           // react to e.id ...
 *       });
 *   bus.publish(SessionStarted{.id = "s1"});   // slot runs here, inline
 *
 * Contract (what the bus promises, and everything it deliberately is not):
 *   dispatch        synchronous — publish() returns after the last slot.
 *                   No queues, no worker threads, no async anything.
 *   ordering        slots for one event type run in registration order.
 *                   Re-subscribing after a disconnect goes to the back.
 *   payload         passed by const reference; the bus never copies it.
 *                   Evt must be a complete type at every call site
 *                   (typeid drives the routing; a forward declaration
 *                   will not compile to anything defined).
 *   threading       every operation may be called concurrently from any
 *                   thread, including from inside a running slot: the
 *                   registry lock is never held while user code runs,
 *                   and boost::signals2 makes a single signal safe under
 *                   concurrent invocation and connection churn. Slots
 *                   always run on whichever thread published.
 *   exceptions      a throwing slot propagates unchanged to the
 *                   publisher — never swallowed, never logged (no
 *                   logging dependency, by design). Slots registered
 *                   after the thrower for that event are NOT invoked.
 *   no subscribers  publishing is a no-op, even for a never-subscribed
 *                   type — publish() never allocates a signal, so an
 *                   event nobody listens to costs one map lookup.
 *
 * The default bus (default_bus()) is a function-local static:
 * initialised on first use, thread-safe, destroyed during static
 * destruction. Slots still connected at that point disconnect in an
 * unspecified order relative to other statics — long-lived subscribers
 * on the default bus should hold a plain Connection and disconnect
 * explicitly before main returns rather than rely on scope handles.
 *
 * Header-only, stateless apart from the per-bus registry.
 */

#include <atomic>       // std::atomic (group counter)
#include <cstddef>      // std::size_t
#include <memory>       // std::shared_ptr, std::static_pointer_cast
#include <mutex>        // std::mutex, std::lock_guard
#include <type_traits>  // std::is_invocable_v
#include <typeindex>    // std::type_index
#include <typeinfo>     // typeid
#include <unordered_map>
#include <utility>      // std::move

#include <boost/signals2/signal.hpp>      // boost::signals2::signal
#include <boost/signals2/connection.hpp>  // connection, scoped_connection

namespace eventbus {

namespace detail {

// ===== per-event-type signal storage ========================================

/// Non-template anchor for the per-event-type storage. Exists only so the
/// registry can hold heterogeneous signals behind one map value type and
/// query/clear them without knowing Evt. The map key (std::type_index) is
/// the sole authority for which derived type an entry is — holder_for<Evt>
/// is the only writer of the map, which is what makes the
/// static_pointer_casts in EventBus provably safe.
struct SignalHolderBase {
    virtual ~SignalHolderBase() = default;

    /// Slots currently connected (boost::signals2 num_slots()).
    virtual std::size_t slot_count() const noexcept = 0;

    /// Disconnect every slot of this event type.
    virtual void disconnect_all() = 0;
};

/// Per-event-type state: the signal itself plus the counter that turns
/// registration order into invocation order (see EventBus::subscribe).
template <typename Evt>
struct SignalHolder final : SignalHolderBase {
    /// void(const Evt&) signature. The Group type is widened to int64 and
    /// the combiner spelled out only to reach the Group parameter (both
    /// are the defaults otherwise, apart from int -> std::int64_t).
    using Signal = boost::signals2::signal<void(const Evt&),
                                           boost::signals2::optional_last_value<void>,
                                           std::int64_t,
                                           std::less<std::int64_t>>;

    /// Next slot-group id for this (bus, event type). Monotonic, only ever
    /// consumed via fetch_add, so ids are handed out in subscribe-call
    /// order — which signals2 turns into invocation order.
    std::atomic<std::int64_t> next_group{0};

    Signal signal;

    std::size_t slot_count() const noexcept override { return signal.num_slots(); }
    void disconnect_all() override { signal.disconnect_all_slots(); }
};

} // namespace detail

// ===== EventBus ==============================================================

class EventBus {
public:
    /// Disconnect handle. Keep it to unsubscribe later (disconnect()), or
    /// wrap in ScopedSubscription for disconnect-on-scope-exit.
    using Connection = boost::signals2::connection;

    /// RAII subscription: disconnects on destruction. Movable, not
    /// copyable, and safe to destroy after the bus is gone (disconnect on
    /// a dead signal is a no-op).
    using ScopedSubscription = boost::signals2::scoped_connection;

    /// One bus is one registry of shared state: copying or moving it would
    /// leave two authorities over the same slots, so both are deleted (as
    /// on endpoint::ModelResponseReader).
    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    /**
     * @brief Register `slot` for events of type Evt.
     * @tparam Evt  event type; always spelled explicitly (it appears in no
     *              function parameter, so it cannot be deduced).
     * @param slot  any callable invocable as slot(const Evt&).
     * @return Connection handle for a later unsubscribe.
     *
     * The slot runs synchronously on the publisher's thread, in
     * registration order, receiving the event by const reference. Safe to
     * call concurrently with any other EventBus operation, including from
     * inside a running slot. Re-subscribing after a disconnect goes to the
     * back of the order.
     */
    template <typename Evt, typename Slot>
    Connection subscribe(Slot slot) {
        static_assert(std::is_invocable_v<Slot&, const Evt&>,
                      "eventbus: slot must be callable as slot(const Evt&) "
                      "— spell the event type explicitly: "
                      "bus.subscribe<MyEvent>(...)");
        const auto holder = holder_for<Evt>();
        const auto group = holder->next_group.fetch_add(1, std::memory_order_relaxed);
        // ALWAYS the grouped connect overload. An ungrouped (two-argument)
        // connect here would place the slot after every grouped slot
        // regardless of registration time and silently break the ordering
        // contract; the monotonic group ids are what make registration
        // order == invocation order.
        return holder->signal.connect(group, std::move(slot));
    }

    /**
     * @brief Invoke every slot registered for Evt, immediately, in order.
     * @tparam Evt  event type of `event`.
     * @param event the payload, passed to each slot by const reference.
     *
     * With no subscribers this is a no-op. Slot exceptions propagate
     * unchanged to the caller, and the remaining slots for this event are
     * not invoked (see the file contract). The registry lock is not held
     * while slots run, so slots may re-enter this bus freely.
     */
    template <typename Evt>
    void publish(const Evt& event) {
        std::shared_ptr<detail::SignalHolder<Evt>> holder;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            const auto found = _holders.find(std::type_index(typeid(Evt)));
            if (found == _holders.end()) {
                return;  // never subscribed: no-op, and no holder is created
            }
            holder = std::static_pointer_cast<detail::SignalHolder<Evt>>(found->second);
        }
        // Lock released. The shared_ptr keeps the signal alive even against
        // a concurrent clear(), and signals2 synchronises the invocation
        // itself — user code (the slots) runs with no registry lock held.
        holder->signal(event);
    }

    /**
     * @brief Slots currently registered for Evt.
     * @tparam Evt  event type to query.
     * @return connected slot count; 0 for a never-subscribed type.
     */
    template <typename Evt>
    std::size_t subscriber_count() const {
        std::shared_ptr<detail::SignalHolderBase> holder;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            const auto found = _holders.find(std::type_index(typeid(Evt)));
            if (found == _holders.end()) {
                return 0;
            }
            holder = found->second;
        }
        return holder->slot_count();
    }

    /**
     * @brief Disconnect every slot of every event type.
     *
     * A concurrent in-flight publish() finishes against the pre-clear slot
     * set: the removed signals stay alive until their last invocation
     * drains (the shared_ptr hand-off in publish() carries them). The bus
     * remains usable — new subscribes start from a clean registry.
     */
    void clear() {
        std::unordered_map<std::type_index, std::shared_ptr<detail::SignalHolderBase>> removed;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            removed.swap(_holders);
        }
        // Disconnect outside the lock: disconnect_all() touches signals2
        // internals only, but the discipline "no signals work under the
        // registry lock" is kept uniform everywhere.
        for (const auto& [type, holder] : removed) {
            holder->disconnect_all();
        }
    }

private:
    /// Find-or-create the holder for Evt. The ONLY writer of the map, which
    /// keeps the invariant "entry under typeid(Evt) IS a
    /// SignalHolder<Evt>" that every static_pointer_cast above relies on.
    /// The lock is held for the map operation only — never across user
    /// code (make_shared allocates, but runs nothing of the caller's).
    template <typename Evt>
    std::shared_ptr<detail::SignalHolder<Evt>> holder_for() {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto [found, inserted] = _holders.emplace(std::type_index(typeid(Evt)), nullptr);
        if (inserted) {
            // Assign after the emplace so a holder already present never
            // allocates a throwaway SignalHolder for the same key.
            found->second = std::make_shared<detail::SignalHolder<Evt>>();
        }
        return std::static_pointer_cast<detail::SignalHolder<Evt>>(found->second);
    }

    /// Guards _holders only. The invariant threaded through every method:
    /// this lock is never held while user code runs (slot invocation, slot
    /// construction) nor while signals2 disconnects — so a slot may freely
    /// call subscribe/publish/clear/subscriber_count on this same bus.
    mutable std::mutex _mutex;

    /// One entry per event type ever subscribed on this bus. Values are
    /// shared_ptrs so publish() can hand the signal out from under the
    /// lock and a clear() racing an in-flight publish stays safe.
    std::unordered_map<std::type_index, std::shared_ptr<detail::SignalHolderBase>> _holders;
};

// ===== default instance ======================================================

/**
 * @brief Process-wide default bus.
 * @return reference to the shared EventBus instance.
 *
 * Function-local static: initialised on first use (thread-safe, immune to
 * static-init-order fiasco) and destroyed during static destruction — see
 * the file contract for the shutdown caveat. Prefer constructing your own
 * EventBus when the bus's lifetime should follow a component instead of
 * the process.
 */
inline EventBus& default_bus() {
    static EventBus bus;
    return bus;
}

} // namespace eventbus
