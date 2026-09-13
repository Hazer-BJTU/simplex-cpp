#include "tools/intrinsic/toolset_base.hpp"

#include <format>
#include <utility>

#include "logging/logger.hpp"

namespace tools::intrinsic {

IntrinsicToolSet::~IntrinsicToolSet()
{
    // The other half of the pair register_tools() opened. release() is noexcept
    // by contract, so nothing here can throw out of a destructor.
    for (const ToolHandle& tool : _tools) {
        tool->release();
    }
}

void IntrinsicToolSet::register_tools(std::vector<ToolHandle> tools)
{
    for (ToolHandle& tool : tools) {
        if (tool == nullptr) {
            logging::Logger::warning(std::format(
                "toolset \"{}\": a null tool was offered and is not "
                "registered", name()));
            continue;
        }

        const std::string tool_name = tool->get_details().name;
        if (tool_name.empty()) {
            // The registry refuses a set offering an unnamed tool (no call
            // could route to it), so catching it here names the set that built
            // it rather than failing later at registration.
            logging::Logger::warning(std::format(
                "toolset \"{}\": a tool with no name is not registered — no "
                "call could route to it", name()));
            continue;
        }
        if (_lookup_table.contains(tool_name)) {
            logging::Logger::warning(std::format(
                "toolset \"{}\": the tool name \"{}\" is offered twice; the "
                "second is not registered — one name must resolve to one tool",
                name(), tool_name));
            continue;
        }

        // build() is the set's business, once per tool (tools/toolsets.hpp) —
        // never part of a call. A tool that refuses is left out of BOTH
        // containers rather than advertised: get_tools() must not list what
        // dispatch() would answer with a half-built tool.
        if (!tool->build()) {
            logging::Logger::warning(std::format(
                "toolset \"{}\": tool \"{}\" refused to build and is not "
                "registered", name(), tool_name));
            continue;
        }

        _lookup_table.emplace(tool_name, tool);
        _tools.push_back(std::move(tool));
    }
}

std::vector<model_io::Invocable> IntrinsicToolSet::get_tools() const
{
    std::vector<model_io::Invocable> catalogue;
    catalogue.reserve(_tools.size());
    for (const ToolHandle& tool : _tools) {
        // Copied: get_details() hands out the tool's own storage, and the
        // caller is building a catalogue it owns.
        catalogue.push_back(tool->get_details());
    }
    return catalogue;
}

IntrinsicToolSet::ToolHandle IntrinsicToolSet::dispatch(
    const model_io::InvokeQuery& query) const
{
    const auto found = _lookup_table.find(query.name);
    if (found == _lookup_table.end()) {
        // nullptr, not a throw: prepare() turns this into the Dispatch-stage
        // record the model reads, and the contract prefers the null here.
        return nullptr;
    }
    return found->second;
}

std::size_t IntrinsicToolSet::tool_count() const noexcept
{
    return _tools.size();
}

} // namespace tools::intrinsic
