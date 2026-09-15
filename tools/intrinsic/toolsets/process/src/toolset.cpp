#include "tools/intrinsic/process/toolset.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

#include "tools/intrinsic/process/schemas.hpp"
#include "tools/intrinsic/process/tools.hpp"

namespace tools::intrinsic {

ProcessToolSet::ProcessToolSet(std::shared_ptr<ProcessSessionStore> store,
                               eventbus::AsyncEventBus* bus)
    : _store(std::move(store))
{
    if (_store == nullptr) {
        throw std::invalid_argument(
            "ProcessToolSet needs a session store: a set with none would "
            "advertise tools it cannot answer");
    }

    // Presentation order, which is what the model reads: launch, then the two
    // observing calls, then the one that acts on a live child — the workflow
    // rather than the alphabet. The base builds each tool, refuses any that
    // will not build, and owns the routing table.
    register_tools({
        std::make_shared<SpawnProcessTool>(_store, bus),
        std::make_shared<PollProcessTool>(_store, bus),
        std::make_shared<ReadProcessTool>(_store, bus),
        std::make_shared<SendProcessTool>(_store, bus),
    });

    // The four are one capability family, and what makes them one is the round
    // trip through a session rather than the file layout: every tool here is
    // either the way an id comes into existence (spawn_process) or something
    // done with one — so a model offered spawn_process WITHOUT send_process can
    // start a process it cannot end, while one offered neither has simply not
    // been given this family. Declaring the group is what makes that difference
    // visible: a partial registration becomes one error line and one
    // capability_groups() answer, instead of four per-tool lines an operator has
    // to assemble (tools/intrinsic/toolset_base.hpp).
    declare_capability_group("process", {
        tool_names::kSpawn,
        tool_names::kPoll,
        tool_names::kRead,
        tool_names::kSend,
    });

    // The set's third job, after its tools and its group: the guidance a model
    // needs to use them TOGETHER — which call comes first, how long to wait for
    // what, what a denied confirmation means (tools/tool_skill.hpp). It lives
    // beside the tool declarations, because it is the same kind of document:
    // written for a model, loaded at run time, no rebuild to change.
    //
    // It is also the one part of the set that is OPTIONAL in the strong sense:
    // a file that cannot be read is reported and leaves the set carrying no
    // skill, with every tool still routable (toolset_base.hpp, load_skill()).
    load_skill(schema_directory() / "skill.yaml");
}

std::string_view ProcessToolSet::name() const noexcept
{
    return kSetName;
}

const std::shared_ptr<ProcessSessionStore>& ProcessToolSet::store() const noexcept
{
    return _store;
}

} // namespace tools::intrinsic
