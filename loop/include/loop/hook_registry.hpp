#pragma once

#include "loop/hook_interface.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loop {

/**
 * Session-level owner of loop hooks and their synchronous subscriptions.
 *
 * The host creates one registry over the EventBus passed to loop::run(). Hooks
 * remain subscribed across run() invocations until removed or replaced. Their
 * process-local state therefore survives turns, while durable state still
 * belongs in AgentInputState. The bus must outlive this registry.
 *
 * Like ToolRegistry configuration, add/set/remove/clear are not synchronized
 * with loop::run() or event publication. Configure hooks before a run or at a
 * serialized boundary; never remove a hook while its callback is executing.
 * The registry does not publish events or alter their callback semantics.
 *
 * Registration order determines callback order for each event type. Replacing
 * a hook disconnects its old slots and appends the replacement's slots after
 * existing subscriptions to the same event type.
 */
class LoopHookRegistry {
public:
    using HookPtr = std::shared_ptr<LoopHookInterface>;

    explicit LoopHookRegistry(eventbus::EventBus& bus) noexcept;
    ~LoopHookRegistry() = default;

    LoopHookRegistry(const LoopHookRegistry&) = delete;
    LoopHookRegistry& operator=(const LoopHookRegistry&) = delete;
    LoopHookRegistry(LoopHookRegistry&&) noexcept = default;
    LoopHookRegistry& operator=(LoopHookRegistry&&) noexcept = default;

    /**
     * Add one named hook and immediately subscribe it to this registry's bus.
     *
     * Rejects null hooks, empty names and duplicate names. A failed bind or
     * insertion leaves existing registrations unchanged and disconnects any
     * subscriptions created by the failed attempt.
     */
    void add(HookPtr hook);

    /**
     * Insert or replace a hook by its name; return true when one was replaced.
     *
     * The replacement binds before the old hook is disconnected, but no event
     * may be published concurrently with this operation. If binding throws,
     * the old hook remains subscribed and registered.
     * Its callbacks append after every still-connected callback of the same
     * event type. If another hook must run after this one (for example, a
     * history-pruning hook after usage accounting), remove the dependent hooks
     * at a serialized boundary, replace this one, then re-add the dependents.
     * Keep owning pointers to them and restore the prior ordered set if a
     * replacement or re-registration fails.
     */
    bool set(HookPtr hook);

    /** Return the named hook, or nullptr if absent. */
    [[nodiscard]] HookPtr get(std::string_view name) const noexcept;

    /** Return whether a hook with this name is registered. */
    [[nodiscard]] bool contains(std::string_view name) const noexcept;

    /** Disconnect and remove the named hook; return false if absent. */
    bool remove(std::string_view name) noexcept;

    /** Disconnect all registered hooks before releasing their instances. */
    void clear() noexcept;

    /** Number of registered hooks. */
    [[nodiscard]] std::size_t size() const noexcept;

    /** Whether the registry has no hooks. */
    [[nodiscard]] bool empty() const noexcept;

    /**
     * Return hooks in registry entry insertion order as owning pointers.
     * set() retains an entry's position but appends its new callbacks to the
     * bus; this enumeration does not describe current callback order.
     */
    [[nodiscard]] std::vector<HookPtr> get_registered() const;

private:
    struct Entry {
        std::string name;
        LoopHookBinding binding;
    };

    [[nodiscard]] std::vector<Entry>::iterator find(std::string_view name) noexcept;
    [[nodiscard]] std::vector<Entry>::const_iterator find(
        std::string_view name) const noexcept;

    eventbus::EventBus* bus_;
    std::vector<Entry> entries_;
};

} // namespace loop
