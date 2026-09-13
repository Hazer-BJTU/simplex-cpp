#pragma once

//
// toolset_base.hpp — the routing and lifecycle every intrinsic toolset repeats
// ===========================================================================
//
// A ToolSet must implement three methods (tools/toolsets.hpp): name(),
// get_tools() and dispatch(). For an in-process set of concrete tools, two of
// those three are the same code every time — a vector in presentation order
// for get_tools(), a map keyed by tool name for dispatch() — and so is the
// build()/release() pair around the tools' own lifecycle. This class is that
// code, so a toolset family supplies only its name and its tools.
//
// WHAT A DERIVED SET DOES. Call register_tools() from its constructor with its
// tools in the order the model should meet them, and return its name from
// name(). Nothing else: get_tools() and dispatch() are final here, because a
// set that needed different routing would not want this base at all.
//
// WHAT THIS DOES NOT OVERRIDE, and why it matters more than what it does:
// prepare() and execute() are left exactly as ToolSet defines them. Those two
// run the module's whole checkpoint sequence and hold its failure contracts —
// prepare() throws only InvokeException, execute() never throws — and an
// in-process toolset has no reason to want a different sequence. Overriding
// them would mean re-deriving those guarantees for nothing.
//
// TWO CONTAINERS, ON PURPOSE. get_tools() must preserve ORDER (it is what the
// model reads, and a set is free to present its tools in the order it wants
// them seen), while dispatch() must be a LOOKUP. Keeping both is the same
// split ToolRegistry makes one level up, for the same reason.
//
// THE build()/release() PAIR IS THE SET'S BUSINESS, not part of any call: a
// set builds each tool once when it takes it on, and releases it when it drops
// it (tools/toolsets.hpp says so explicitly — neither phase of a call runs
// them). A tool whose build() refuses is left out of BOTH containers, so it
// never appears in the catalogue and no call can route to it: advertising a
// tool that failed to build would promise the model something dispatch()
// would then have to answer with a half-built object.
//

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dataclass/model_io.hpp"
#include "tools/toolsets.hpp"

namespace tools::intrinsic {

/**
 * The base an intrinsic toolset derives from: the ordered catalogue, the
 * routing table over it, and the tools' build/release lifecycle.
 *
 * A derived set implements name() and calls register_tools() in its
 * constructor. See the file header for what is deliberately left to the
 * ToolSet base (prepare/execute) and why.
 */
class IntrinsicToolSet : public tools::ToolSet {
public:
    ~IntrinsicToolSet() override;

    IntrinsicToolSet(const IntrinsicToolSet&) = delete;
    IntrinsicToolSet& operator = (const IntrinsicToolSet&) = delete;
    IntrinsicToolSet(IntrinsicToolSet&&) = delete;
    IntrinsicToolSet& operator = (IntrinsicToolSet&&) = delete;

    /// The catalogue, in the order register_tools() was given — which is what
    /// the model reads, so a set orders it by workflow rather than by name.
    std::vector<model_io::Invocable> get_tools() const final;

    /// The tool for `query.name`, or nullptr when this set has none — which
    /// prepare() reports as a Dispatch failure. Returning the null is what the
    /// contract asks for here, rather than throwing.
    ToolHandle dispatch(const model_io::InvokeQuery& query) const final;

    /// How many tools the set actually took on. Not necessarily how many were
    /// offered to register_tools(): one whose build() refused is not here.
    [[nodiscard]] std::size_t tool_count() const noexcept;

protected:
    IntrinsicToolSet() = default;

    /**
     * Take on `tools`, in presentation order — the derived constructor's one
     * job.
     *
     * Each tool is built (ToolInterface::build()); one that refuses is logged
     * and left out entirely. A tool with an empty name, or a name another tool
     * in the same set already offered, is also refused: the registry would
     * reject the whole set for either (one name must resolve to one tool), and
     * failing here names the offending set instead of failing at registration.
     *
     * May be called more than once; later calls append.
     */
    void register_tools(std::vector<ToolHandle> tools);

private:
    /// Presentation order for get_tools().
    std::vector<ToolHandle> _tools;
    /// Name -> tool for dispatch().
    std::unordered_map<std::string, ToolHandle> _lookup_table;
};

} // namespace tools::intrinsic
