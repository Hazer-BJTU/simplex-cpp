#include "tools/intrinsic/toolset_base.hpp"

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "logging/logger.hpp"
#include "tools/intrinsic/skill_declaration.hpp"

namespace tools::intrinsic {
namespace {

/// The names, comma-separated, in the order they were declared — how a report
/// about a group spells its members.
[[nodiscard]] std::string joined(const std::vector<std::string>& names)
{
    std::string text;
    for (const std::string& name : names) {
        if (!text.empty()) text += ", ";
        text += name;
    }
    return text;
}

} // namespace

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

std::optional<tools::ToolSetSkill> IntrinsicToolSet::skill() const
{
    // Copied, not referenced: ToolSet::skill() answers by value, and the skill
    // is read by a host that may keep it past the next registration.
    return _skill;
}

void IntrinsicToolSet::load_skill(const std::filesystem::path& file)
{
    // The loader reports its own refusal (missing file, YAML error, a document
    // without a name or without text), so this only has to decide what the set
    // does about it: nothing. The tools stay routable and the model is still
    // told what each one does — guidance is not a capability.
    if (std::optional<tools::ToolSetSkill> loaded =
            try_load_skill_declaration(file)) {
        // Only on success, so a later file that fails to load cannot take an
        // earlier skill away from the set.
        _skill = std::move(loaded);
    }
}

std::vector<IntrinsicToolSet::CapabilityGroup> IntrinsicToolSet::capability_groups() const
{
    std::vector<CapabilityGroup> groups;
    groups.reserve(_groups.size());
    for (const DeclaredGroup& group : _groups) {
        // Looked up now rather than remembered at declaration time: a set that
        // registers more tools afterwards is answered about the set it has.
        CapabilityGroup status;
        status.name = group.name;
        for (const std::string& tool : group.tools) {
            if (_lookup_table.contains(tool)) {
                status.registered.push_back(tool);
            } else {
                status.missing.push_back(tool);
            }
        }
        groups.push_back(std::move(status));
    }
    return groups;
}

void IntrinsicToolSet::declare_capability_group(
    std::string_view group, std::vector<std::string_view> tools)
{
    DeclaredGroup declared;
    declared.name = std::string(group);
    declared.tools.reserve(tools.size());
    for (const std::string_view tool : tools) {
        declared.tools.emplace_back(tool);
    }
    _groups.push_back(std::move(declared));

    // Report at declaration time, so the line lands in the log of the
    // construction that produced the state rather than whenever a host happens
    // to ask. What it says is deliberately concrete — which family, how much of
    // it, and by name what is gone — because the alternative is an operator
    // reading a per-tool line for each member and working out the shape
    // themselves.
    const std::vector<CapabilityGroup> groups = capability_groups();
    const CapabilityGroup& status = groups.back();
    if (status.missing.empty()) return;

    const std::string missing = joined(status.missing);
    if (status.registered.empty()) {
        logging::Logger::error(std::format(
            "toolset \"{}\": capability group \"{}\" registered NONE of its {} "
            "tools (missing {}): the whole family is absent, so a model is "
            "offered none of it", name(), status.name, status.missing.size(),
            missing));
        return;
    }
    logging::Logger::error(std::format(
        "toolset \"{}\": capability group \"{}\" is DEGRADED — {} of its {} "
        "tools registered, missing {}; the rest stay routable, which is a "
        "partial capability rather than none", name(), status.name,
        status.registered.size(),
        status.registered.size() + status.missing.size(), missing));
}

} // namespace tools::intrinsic
