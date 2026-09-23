#pragma once

#include "eventbus/event_bus.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace loop {

class LoopHookInterface;

/**
 * Owns one hook's synchronous event subscriptions and its plugin instance.
 *
 * Destroying the binding disconnects its subscriptions before releasing the
 * plugin. Keep the binding alive for as long as the hook should receive events.
 * A binding may be moved, but cannot be copied. The bus must outlive it.
 *
 * Subscription changes must be serialized with loop::run(). In particular,
 * do not destroy a binding while one of its callbacks is executing. The event
 * bus permits concurrent disconnect, but that alone does not join an in-flight
 * callback or make destruction of the plugin's state safe.
 */
class LoopHookBinding {
public:
    LoopHookBinding(LoopHookBinding&&) noexcept = default;
    LoopHookBinding& operator=(LoopHookBinding&& other) noexcept;

    LoopHookBinding(const LoopHookBinding&) = delete;
    LoopHookBinding& operator=(const LoopHookBinding&) = delete;

    ~LoopHookBinding() = default;

    /** The hook kept alive by this binding, or nullptr after it is moved. */
    [[nodiscard]] std::shared_ptr<LoopHookInterface> hook() const noexcept;

private:
    friend class LoopHookInterface;

    using Subscriptions = std::vector<eventbus::EventBus::ScopedSubscription>;

    LoopHookBinding(std::shared_ptr<LoopHookInterface> hook,
                    Subscriptions subscriptions) noexcept;

    // Members are destroyed in reverse order: disconnect first, then release
    // the hook that may be captured by its callbacks.
    std::shared_ptr<LoopHookInterface> hook_;
    Subscriptions subscriptions_;
};

/**
 * Common interface for all loop event-hook plugins, including built-in ones.
 *
 * The host constructs a concrete hook in a shared_ptr and attaches it to an
 * explicitly chosen synchronous EventBus. The hook may keep instance state
 * across events and run() invocations. It is not itself persistent: any state
 * needed after process restart belongs in model_io::AgentInputState.
 *
 * A hook observes or edits events under the contracts in loop/events.hpp.
 * Callbacks run inline on the publishing thread and must not retain event
 * references, access the live state asynchronously, or reenter loop::run().
 * Writable events still undergo the loop's validation and rollback rules.
 * Exceptions propagate according to the event's documented behavior.
 *
 * The interface does not depend on extensions or a process-wide event bus.
 */
class LoopHookInterface {
public:
    using Subscriptions = std::vector<eventbus::EventBus::ScopedSubscription>;

    virtual ~LoopHookInterface();

    LoopHookInterface(const LoopHookInterface&) = delete;
    LoopHookInterface& operator=(const LoopHookInterface&) = delete;

    /** Stable diagnostic name; its view must remain valid for this instance. */
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /**
     * Bind a shared hook to the given bus and return its lifetime handle.
     *
     * A null hook is rejected. Each call creates an independent binding; the
     * host should normally bind an instance once, since multiple bindings
     * register its callbacks multiple times. The host must keep bus alive and
     * serialize binding changes with loop::run().
     */
    [[nodiscard]] static LoopHookBinding attach(
        std::shared_ptr<LoopHookInterface> hook,
        eventbus::EventBus& bus);

protected:
    LoopHookInterface() = default;

    /**
     * Register this hook's event callbacks and return all scoped connections.
     *
     * Adopt every new bus connection into a local ScopedSubscription before
     * inserting it into the collection. A vector allocation can throw after
     * subscribe() connects the callback; a raw Connection temporary would
     * leave that callback attached to a potentially destroyed instance.
     * For example:
     *
     *   eventbus::EventBus::ScopedSubscription owned{
     *       bus.subscribe<MyEvent>([this](const MyEvent& event) { handle(event); })};
     *   subscriptions.push_back(std::move(owned));
     *
     * Callbacks may capture this; the returned
     * LoopHookBinding keeps the instance alive until they are disconnected.
     * Do not retain the bus or the event payload in the hook.
     */
    [[nodiscard]] virtual Subscriptions subscribe(eventbus::EventBus& bus) = 0;
};

} // namespace loop
