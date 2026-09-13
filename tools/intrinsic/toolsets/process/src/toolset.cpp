#include "tools/intrinsic/process/toolset.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

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
    // observing calls, then the ones that act on a live child — the workflow
    // rather than the alphabet. The base builds each tool, refuses any that
    // will not build, and owns the routing table.
    register_tools({
        std::make_shared<SpawnProcessTool>(_store, bus),
        std::make_shared<PollProcessesTool>(_store, bus),
        std::make_shared<ReadProcessOutputTool>(_store, bus),
        std::make_shared<WaitProcessTool>(_store, bus),
        std::make_shared<WriteProcessInputTool>(_store, bus),
        std::make_shared<KillProcessTool>(_store, bus),
    });
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
