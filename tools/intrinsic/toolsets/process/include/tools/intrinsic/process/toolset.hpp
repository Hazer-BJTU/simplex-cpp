#pragma once

//
// toolset.hpp — the process tools as one routable set
// ===================================================
//
// What a host registers with its ToolRegistry to give a model process
// management: one ToolSet over the six tools in tools.hpp, sharing one
// ProcessSessionStore.
//
// Almost nothing is left to do here. IntrinsicToolSet
// (tools/intrinsic/toolset_base.hpp) already carries the ordered catalogue,
// the routing table, and the tools' build/release lifecycle, and the ToolSet
// base carries prepare()/execute() with the module's checkpoint sequence and
// failure contracts. So this class is its name, its six tools, and the store
// they share.
//
// THE STORE IS SHARED, AND OUTLIVES THE SET IF IT HAS TO. It is taken as a
// shared_ptr because a host may well hold the same store elsewhere — to
// terminate everything at shutdown (see the store's terminate_all(), which is
// the shutdown path), or to show a live process list in a UI — and because the
// store's own coroutines outlast any single call. Handing the set a store
// rather than an executor is also what makes the set testable: a test builds a
// store on its own io_context and sees exactly the sessions its calls made.
//

#include <memory>
#include <string_view>

#include "eventbus/async_event_bus.hpp"
#include "tools/intrinsic/process/session_store.hpp"
#include "tools/intrinsic/toolset_base.hpp"

namespace tools::intrinsic {

/**
 * The process-management tool set: six tools over one session store.
 *
 * Registered with a ToolRegistry by the host that owns it
 * (`registry.add(std::make_shared<ProcessToolSet>(store))`).
 */
class ProcessToolSet final : public IntrinsicToolSet {
public:
    /// The set's name, as failure records and logs spell it.
    static constexpr std::string_view kSetName = "process";

    /**
     * Build the set over `store`.
     *
     * @param store the session table the tools work through; must not be
     *        null — a set with no store could route calls it cannot answer.
     * @param bus the bus the RequireConfirm tools ask their confirmation
     *        question on; nullptr (the default) means the process-wide
     *        eventbus::default_async_bus(), which is what a host wants — a
     *        confirmer in another module can then answer. Pass one to keep a
     *        component's confirmations to itself. Borrowed: it must outlive
     *        the set.
     * @throws std::invalid_argument for a null store.
     */
    explicit ProcessToolSet(std::shared_ptr<ProcessSessionStore> store,
                            eventbus::AsyncEventBus* bus = nullptr);

    std::string_view name() const noexcept override;

    /// The store the tools share, for a host that also wants to observe or
    /// terminate sessions directly.
    [[nodiscard]] const std::shared_ptr<ProcessSessionStore>& store() const noexcept;

private:
    std::shared_ptr<ProcessSessionStore> _store;
};

} // namespace tools::intrinsic
