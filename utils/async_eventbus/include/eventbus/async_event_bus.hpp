#pragma once

/**
 * @file eventbus/async_event_bus.hpp
 * @brief Type-dispatched serial-async publish/subscribe bus (coroutine fold).
 *
 * The asynchronous counterpart of eventbus/event_bus.hpp: an event is a struct
 * whose C++ type IS the topic, and subscribe/publish are typed by that struct.
 * The difference is the dispatch model — handlers are Boost.Asio coroutines,
 * and publish() FOLDS the event through them one at a time instead of fanning
 * it out:
 *
 *   out = co_await handler_1(in);
 *   out = co_await handler_2(out);
 *   ...
 *   co_return out;
 *
 * So the input Evt is copied once, the first handler's co_return value becomes
 * the second handler's input, and publish() returns the final Evt. Most events
 * only need a single field rewritten; a handler that must append to a list
 * result simply keeps a container (e.g. std::vector) INSIDE Evt and pushes into
 * it — no separate list-of-results return type is needed.
 *
 * Unlike the synchronous bus, an event type is NOT a bare struct: it must
 * derive from eventbus::AsyncEventBase, whose `errors` container is what lets
 * capture-mode publish keep ONE unified return type (see below). The
 * AsyncEventType concept enforces this at the template boundary.
 *
 * Usage:
 *   struct SessionStarted : eventbus::AsyncEventBase {
 *       std::string id;
 *       std::vector<std::string> log;
 *   };
 *
 *   auto& bus = eventbus::default_async_bus();
 *   eventbus::AsyncEventBus::ScopedSubscription sub =
 *       bus.subscribe<SessionStarted>(
 *           [](const SessionStarted& e) -> boost::asio::awaitable<SessionStarted> {
 *               SessionStarted out = e;            // copies `errors` too
 *               out.log.push_back("seen by handler");
 *               co_return out;                     // becomes the next input
 *           });
 *
 *   SessionStarted result = co_await bus.publish(SessionStarted{.id = "s1"});
 *
 * Contract (what the bus promises, and everything it deliberately is not):
 *   dispatch        serial coroutine fold — publish() co_awaits every live
 *                   handler for Evt in registration order, each handler's
 *                   co_return value becoming the next handler's argument, and
 *                   returns the last value. No queues, no worker threads, no
 *                   concurrency between handlers of one publish.
 *   ordering        handlers for one event type run in registration order;
 *                   re-subscribing after a disconnect goes to the back.
 *   payload         publish(const Evt&) COPIES the event and passes that copy
 *                   (by const reference) to the first handler; the caller's
 *                   event is never mutated. Each handler receives the previous
 *                   handler's output and returns a new Evt by value.
 *   no subscribers  publish() returns a copy of the input unchanged.
 *   exceptions      two modes, selected by a token parameter, with ONE unified
 *                   return type (awaitable<Evt>):
 *                     - publish(evt) / publish(evt, propagate_exceptions):
 *                       a throwing handler propagates unchanged to the
 *                       publisher and the remaining handlers are NOT invoked.
 *                     - publish(evt, capture_exceptions): every handler runs;
 *                       a throwing handler's exception is collected and the
 *                       fold continues with the value that was current before
 *                       the throw. The returned event's AsyncEventBase::errors
 *                       is the input's pre-existing errors (preserved) plus one
 *                       std::exception_ptr per throw, in order. This is
 *                       bus-owned, final publisher-facing metadata: written
 *                       only when the fold completes, so it survives a handler
 *                       that returns a freshly constructed event, and handlers
 *                       do not observe this run's captured errors mid-fold.
 *                   Never swallowed silently, never logged (no logging
 *                   dependency, by design).
 *   threading       subscribe/subscriber_count/clear may be called concurrently
 *                   from any thread, including from inside a running handler.
 *                   The registry mutex is NEVER held across a co_await, so a
 *                   handler may freely re-enter this bus (subscribe or nested
 *                   publish). Concurrent publishes are independent: each
 *                   snapshots the slot list and folds serially on its own; the
 *                   bus does not serialize one publish against another.
 *   disconnect      stops FUTURE dispatches from starting that handler; a
 *                   handler already in flight runs to completion (coroutines
 *                   are not cancelled mid-flight). Dead slots are pruned
 *                   lazily on the next subscribe and all at once on clear().
 *
 * The default bus (default_async_bus()) is a function-local static initialised
 * on first use, thread-safe, destroyed during static destruction. As with the
 * synchronous bus, the singleton is compiled into a SHARED library
 * (async_eventbus_lib) so every executable and dlopened plugin binds ONE bus
 * per process by SONAME — an inline singleton would fork a private bus inside
 * every module and events would vanish at the boundary.
 *
 * The AsyncEventBus class itself is header-inline and stateless apart from the
 * per-bus registry; only default_async_bus() is compiled. Unlike the signals2
 * bus it depends on Boost.Asio (consumers link async_eventbus_iface → asio_iface,
 * which is the one-compiled-copy-of-asio rule), never on logging_lib.
 */

#include <algorithm>     // std::remove_if
#include <atomic>        // std::atomic
#include <concepts>      // std::derived_from
#include <cstddef>       // std::size_t
#include <exception>     // std::exception_ptr, std::current_exception
#include <functional>    // std::function
#include <memory>        // std::shared_ptr, std::static_pointer_cast
#include <mutex>         // std::mutex, std::lock_guard
#include <type_traits>   // std::invoke_result_t
#include <typeindex>     // std::type_index
#include <typeinfo>      // typeid
#include <unordered_map>
#include <utility>       // std::move
#include <vector>

#include <boost/asio.hpp>  // boost::asio::awaitable, this_coro, use_awaitable

namespace eventbus {

// ===== event base + type constraint ==========================================

/**
 * @brief Base class every async event type must derive from.
 *
 * Carries the errors container that capture-mode publish() fills: the input
 * event's pre-existing exceptions plus one std::exception_ptr per throwing
 * handler, in execution order. This is bus-owned, final publisher-facing
 * metadata — capture-mode publish() collects exceptions into a local list and
 * assigns it to the returned event's `errors` only when the fold completes, so
 * handlers never have to carry errors forward themselves (a handler that
 * returns a freshly constructed event instead of copying its input is fine),
 * and they do not observe this run's newly captured exceptions mid-fold.
 */
struct AsyncEventBase {
    /// Exceptions recorded by capture-mode publish, in execution order.
    std::vector<std::exception_ptr> errors;
};

/**
 * @brief Constrains an event type to one that derives from AsyncEventBase.
 *
 * Every event type used with AsyncEventBus must satisfy this. It is what lets
 * capture-mode publish keep one unified return type — the event itself carries
 * its errors, so both modes return awaitable<Evt>.
 */
template <typename T>
concept AsyncEventType = std::derived_from<T, AsyncEventBase>;

/**
 * @brief Constrains a callable to one usable as an async handler for Evt.
 *
 * A handler must be copy-constructible (it is stored in a std::function),
 * invocable as slot(const Evt&), and return boost::asio::awaitable<Evt>. This
 * one concept replaces the three static_asserts that used to guard subscribe().
 */
template <typename Slot, typename Evt>
concept AsyncHandler =
    AsyncEventType<Evt> &&
    std::copy_constructible<Slot> &&
    std::invocable<Slot&, const Evt&> &&
    std::same_as<std::invoke_result_t<Slot&, const Evt&>,
                 boost::asio::awaitable<Evt>>;

// ===== exception-mode tokens =================================================

/**
 * @brief Propagate mode: a throwing handler aborts the fold and the exception
 *        propagates unchanged to the publisher. This is publish()'s default.
 */
struct propagate_exceptions_t {};
inline constexpr propagate_exceptions_t propagate_exceptions{};

/**
 * @brief Capture mode: every handler runs; a throwing handler's exception is
 *        pushed into the event's AsyncEventBase::errors and the fold continues.
 */
struct capture_exceptions_t {};
inline constexpr capture_exceptions_t capture_exceptions{};

namespace detail {

// ===== per-event-type slot storage ===========================================

/// Non-template anchor so the registry can hold heterogeneous holders behind
/// one map value type. The map key (std::type_index) is the sole authority for
/// which derived type an entry is — holder_for<Evt> is the only writer, which
/// is what makes the static_pointer_casts in AsyncEventBus provably safe.
struct SlotStateBase {
    virtual ~SlotStateBase() = default;

    /// Mark the slot disconnected (idempotent). Stops FUTURE dispatches only.
    virtual void disconnect() noexcept = 0;

    /// Whether the slot is still connected.
    virtual bool connected() const noexcept = 0;
};

/// One registered handler: a copyable callable stored in std::function, plus
/// the live/dead flag the Connection toggles. Invoking fn(e) produces a fresh
/// awaitable<Evt> (a new coroutine frame) every call — the awaitable itself is
/// move-only and therefore never stored.
template <typename Evt>
struct SlotState final : SlotStateBase {
    std::atomic<bool> _connected{true};
    std::function<boost::asio::awaitable<Evt>(const Evt&)> fn;

    void disconnect() noexcept override {
        _connected.store(false, std::memory_order_release);
    }
    bool connected() const noexcept override {
        return _connected.load(std::memory_order_acquire);
    }
};

struct HolderBase {
    virtual ~HolderBase() = default;

    /// Number of LIVE slots (disconnected slots not counted).
    virtual std::size_t slot_count() const noexcept = 0;

    /// Disconnect every slot of this event type.
    virtual void disconnect_all() = 0;

    /// Drop dead slots, compacting the vector.
    virtual void prune_dead() = 0;
};

/// Per-event-type state: the ordered slot vector. Order is registration order;
/// disconnect leaves a tombstone (flag false) that prune_dead() reaps.
template <typename Evt>
struct Holder final : HolderBase {
    std::vector<std::shared_ptr<SlotState<Evt>>> slots;

    std::size_t slot_count() const noexcept override {
        std::size_t n = 0;
        for (const auto& s : slots) {
            if (s->connected()) {
                ++n;
            }
        }
        return n;
    }

    void disconnect_all() override {
        for (auto& s : slots) {
            s->disconnect();
        }
    }

    void prune_dead() override {
        slots.erase(
            std::remove_if(slots.begin(), slots.end(),
                           [](const std::shared_ptr<SlotState<Evt>>& s) {
                               return !s->connected();
                           }),
            slots.end());
    }
};

} // namespace detail

// ===== AsyncEventBus =========================================================

class AsyncEventBus {
public:
    /// Disconnect handle. Keep it to unsubscribe later (disconnect()), or wrap
    /// in ScopedSubscription for disconnect-on-scope-exit. Copyable, like the
    /// synchronous bus's signals2 connection; disconnect() is idempotent.
    class Connection {
    public:
        Connection() = default;
        explicit Connection(std::shared_ptr<detail::SlotStateBase> state)
            : _state(std::move(state)) {}

        /// Unsubscribe. Idempotent; stops FUTURE dispatches only. Const so a
        /// `const auto` handle can still disconnect (as on signals2).
        void disconnect() const {
            if (_state) {
                _state->disconnect();
            }
        }

        /// True iff the slot is still connected (false for a default-constructed
        /// handle and for a disconnected one).
        [[nodiscard]] bool connected() const noexcept {
            return _state && _state->connected();
        }

        /// True iff this handle is bound to a slot (connected or not).
        explicit operator bool() const noexcept { return _state != nullptr; }

    private:
        std::shared_ptr<detail::SlotStateBase> _state;
    };

    /// RAII subscription: disconnects on destruction. Movable, not copyable,
    /// and safe to destroy after the bus is gone (disconnect on a dead slot is
    /// a no-op).
    class ScopedSubscription {
    public:
        ScopedSubscription() = default;
        // Implicit from Connection so `ScopedSubscription sub = bus.subscribe(...)`
        // works, exactly as signals2's scoped_connection is built from a connection.
        ScopedSubscription(Connection conn) : _conn(std::move(conn)) {}
        ScopedSubscription(ScopedSubscription&&) = default;
        ScopedSubscription& operator=(ScopedSubscription&&) = default;
        ScopedSubscription(const ScopedSubscription&) = delete;
        ScopedSubscription& operator=(const ScopedSubscription&) = delete;
        ~ScopedSubscription() { _conn.disconnect(); }

        void disconnect() { _conn.disconnect(); }
        [[nodiscard]] bool connected() const noexcept { return _conn.connected(); }

    private:
        Connection _conn;
    };

    /// One bus is one registry of shared state: copying or moving it would
    /// leave two authorities over the same slots, so both are deleted (as on
    /// EventBus and endpoint::ModelResponseReader).
    AsyncEventBus() = default;
    ~AsyncEventBus() = default;
    AsyncEventBus(const AsyncEventBus&) = delete;
    AsyncEventBus& operator=(const AsyncEventBus&) = delete;
    AsyncEventBus(AsyncEventBus&&) = delete;
    AsyncEventBus& operator=(AsyncEventBus&&) = delete;

    /**
     * @brief Register `slot` for events of type Evt.
     * @tparam Evt  event type; always spelled explicitly (it appears in no
     *              function parameter, so it cannot be deduced). Must derive
     *              from eventbus::AsyncEventBase (AsyncEventType).
     * @tparam Slot a callable constrained by eventbus::AsyncHandler<Evt>:
     *              copy-constructible, invocable as slot(const Evt&), and
     *              returning boost::asio::awaitable<Evt>.
     * @return Connection handle for a later unsubscribe.
     *
     * Handlers run serially during publish, in registration order, each
     * receiving the previous handler's co_return value (the first receives the
     * published event) by const reference and returning a new Evt. Safe to call
     * concurrently with any other operation, including from inside a running
     * handler. Re-subscribing after a disconnect goes to the back.
     */
    template <typename Evt, AsyncHandler<Evt> Slot>
    Connection subscribe(Slot slot) {
        auto state = std::make_shared<detail::SlotState<Evt>>();
        state->fn = std::move(slot);

        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto holder = holder_for_locked<Evt>();
            // Reap dead slots lazily so long-lived subscribe/disconnect churn
            // does not grow the vector without bound.
            holder->prune_dead();
            holder->slots.push_back(state);
        }
        return Connection(state);
    }

    /**
     * @brief Fold the event through every live handler, serially, and return
     *        the final value (propagate mode — the default).
     * @tparam Evt event type of `event`; must derive from AsyncEventBase.
     * @param event the payload; copied once, never mutated.
     * @return co_await it for the final folded Evt.
     *
     * A throwing handler propagates unchanged to the publisher and the
     * remaining handlers are not invoked. With no subscribers this returns a
     * copy of `event` unchanged. The registry lock is not held while handlers
     * run, so handlers may re-enter this bus freely.
     */
    template <AsyncEventType Evt>
    boost::asio::awaitable<Evt> publish(const Evt& event) {
        auto snapshot = snapshot_slots<Evt>();
        Evt current = event;  // copy of the input, fed to the first handler
        for (const auto& slot : snapshot) {
            if (slot->connected()) {
                current = co_await slot->fn(current);
            }
        }
        co_return current;
    }

    /**
     * @brief Explicit propagate mode; identical to publish(evt).
     */
    template <AsyncEventType Evt>
    boost::asio::awaitable<Evt> publish(const Evt& event, propagate_exceptions_t) {
        co_return co_await publish(event);
    }

    /**
     * @brief Fold the event through every live handler, capturing exceptions
     *        into the event's inherited AsyncEventBase::errors.
     * @tparam Evt event type of `event`; must derive from AsyncEventBase.
     * @param event the payload; copied once, never mutated.
     * @param token eventbus::capture_exceptions — selects capture mode.
     * @return co_await it for Evt — the same unified return type as propagate
     *         mode, with one std::exception_ptr per throwing handler appended
     *         to `errors`, in execution order.
     *
     * Every live handler runs. If a handler throws, its exception is collected
     * and the fold continues with the value that was current before the throw.
     * The returned event's `errors` is the input event's pre-existing errors
     * (preserved, never silently discarded) plus this run's exceptions. `errors`
     * is final publisher-facing metadata: the bus writes it only when the fold
     * completes, so handlers do NOT observe this run's newly captured exceptions
     * during the fold (they still see whatever the input already carried).
     */
    template <AsyncEventType Evt>
    boost::asio::awaitable<Evt> publish(const Evt& event, capture_exceptions_t) {
        auto snapshot = snapshot_slots<Evt>();
        Evt current = event;
        // Bus-owned metadata: start from the input's pre-existing errors so they
        // are never silently discarded, then append this run's exceptions.
        std::vector<std::exception_ptr> errors = event.errors;
        for (const auto& slot : snapshot) {
            if (!slot->connected()) {
                continue;
            }
            try {
                current = co_await slot->fn(current);
            } catch (...) {
                errors.push_back(std::current_exception());
                // `current` keeps the pre-throw value and is passed to the next handler
            }
        }
        current.errors = std::move(errors);
        co_return current;
    }

    /**
     * @brief Live slots currently registered for Evt.
     * @tparam Evt event type to query; must derive from AsyncEventBase.
     * @return connected handler count; 0 for a never-subscribed type.
     */
    template <AsyncEventType Evt>
    std::size_t subscriber_count() const {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = _holders.find(std::type_index(typeid(Evt)));
        if (found == _holders.end()) {
            return 0;
        }
        const auto holder =
            std::static_pointer_cast<detail::Holder<Evt>>(found->second);
        return holder->slot_count();
    }

    /**
     * @brief Disconnect every handler of every event type.
     *
     * A concurrent in-flight publish() keeps its snapshot alive (the shared_ptr
     * hand-off) and simply skips now-disconnected handlers for the rest of its
     * walk; a handler already started completes. The bus remains usable — new
     * subscribes start from a clean registry.
     */
    void clear() {
        std::unordered_map<std::type_index, std::shared_ptr<detail::HolderBase>> removed;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            removed.swap(_holders);
        }
        // Disconnect outside the lock, keeping the "no user code under the
        // registry lock" discipline uniform everywhere.
        for (const auto& [type, holder] : removed) {
            holder->disconnect_all();
        }
    }

private:
    /// Find-or-create the holder for Evt. The ONLY writer of the map, which
    /// keeps the invariant "entry under typeid(Evt) IS a Holder<Evt>" that
    /// every static_pointer_cast relies on. CALLER MUST HOLD _mutex.
    template <typename Evt>
    std::shared_ptr<detail::Holder<Evt>> holder_for_locked() {
        const auto [found, inserted] =
            _holders.emplace(std::type_index(typeid(Evt)), nullptr);
        if (inserted) {
            found->second = std::make_shared<detail::Holder<Evt>>();
        }
        return std::static_pointer_cast<detail::Holder<Evt>>(found->second);
    }

    /// Copy the slot vector for Evt under the lock and hand it out. The caller
    /// then co_awaits the handlers with NO lock held.
    template <typename Evt>
    std::vector<std::shared_ptr<detail::SlotState<Evt>>> snapshot_slots() {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = _holders.find(std::type_index(typeid(Evt)));
        if (found == _holders.end()) {
            return {};
        }
        const auto holder =
            std::static_pointer_cast<detail::Holder<Evt>>(found->second);
        return holder->slots;  // copies the shared_ptrs, cheap
    }

    /// Guards _holders only. Never held while user code runs (handler
    /// invocation) — the co_await sites in publish are all outside the lock.
    mutable std::mutex _mutex;

    /// One entry per event type ever subscribed on this bus. Values are
    /// shared_ptrs so publish() can hand the slot vector out from under the
    /// lock and a clear() racing an in-flight publish stays safe.
    std::unordered_map<std::type_index, std::shared_ptr<detail::HolderBase>> _holders;
};

// ===== default instance ======================================================

/**
 * @brief Process-wide default async bus.
 * @return reference to the shared AsyncEventBus instance.
 *
 * Deliberately NOT inline: the definition lives in async_eventbus_lib (SHARED,
 * src/default_async_bus.cpp), so every executable AND every dlopened plugin
 * that links that library binds to ONE bus instance by SONAME — the same
 * medicine eventbus_lib applies to default_bus(). Prefer constructing your own
 * AsyncEventBus when the bus's lifetime should follow a component.
 */
AsyncEventBus& default_async_bus();

} // namespace eventbus
