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
// A TOOL THAT DOES NOT ARRIVE COSTS ITSELF AND NO MORE, which is the right
// blast radius for one broken thing (a declaration file that cannot be read,
// tool_declaration.hpp) but not always a safe STATE to leave a model in. Six
// process tools are a capability family: a model offered spawn_process without
// kill_process can start a process it cannot end. So a set may DECLARE its
// families — declare_capability_group() — and is then held to this: a family
// that came out PARTIAL is reported as one error line naming the group and
// every tool it is missing, and is answered by capability_groups() for a host
// that wants to do something about it (refuse to start, show a banner, drop
// the set). Wholly present and wholly absent are the two quiet answers: the
// difference the report exists to draw is "this family is not offered" versus
// "half of it is".
//
// Registration stays per tool deliberately. Registering a group ATOMICALLY —
// dropping the five that did arrive because the sixth did not — would trade a
// degraded toolset for none at all, and a host that wants that trade can read
// capability_groups() and drop the set itself. What this class will not do is
// let the degraded state pass unremarked.
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

    /**
     * One declared capability group and how it came out.
     *
     * `registered` and `missing` together are the whole group, in the order it
     * was declared, so a caller can report it without knowing the family.
     */
    struct CapabilityGroup {
        /// The name the set declared the group under.
        std::string name;
        /// The members that are routable right now.
        std::vector<std::string> registered;
        /// The members that are not — what makes the state worth reporting.
        std::vector<std::string> missing;
    };

    /**
     * Every capability group this set declared, with its current state.
     *
     * Empty for a set that declared none. A group with `missing` empty is whole;
     * one with everything missing is a family the set does not offer at all; one
     * with some of each is the DEGRADED state — the case declare_capability_group()
     * logs about, and the reason a host reads this.
     *
     * Computed from the routing table at call time rather than recorded when the
     * group was declared, so a set that registers more tools afterwards (which
     * register_tools() allows) is answered about the set it has, not the set it
     * had.
     */
    [[nodiscard]] std::vector<CapabilityGroup> capability_groups() const;

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

    /**
     * Declare that `tools` are one capability family, called after
     * register_tools() by the set that owns them (see the file header for what
     * a group is for).
     *
     * The state is reported HERE, once, rather than left for a caller to notice:
     * a group that is only partly registered is an error line naming the group,
     * the count and every missing member, and a group none of whose tools
     * registered is an error line saying the whole family is absent. Both are
     * also answered by capability_groups(), which is what a host acts on.
     *
     * Nothing is enforced: the tools that did register stay routable. See the
     * file header for why the alternative — registering the family atomically —
     * is the host's call rather than this class's.
     */
    void declare_capability_group(std::string_view group,
                                  std::vector<std::string_view> tools);

private:
    /// One declared group, as the set declared it: the name and the whole
    /// membership list, in declaration order.
    struct DeclaredGroup {
        std::string name;
        std::vector<std::string> tools;
    };

    /// Presentation order for get_tools().
    std::vector<ToolHandle> _tools;
    /// Name -> tool for dispatch().
    std::unordered_map<std::string, ToolHandle> _lookup_table;
    /// The capability families this set declared, in the order it declared them.
    std::vector<DeclaredGroup> _groups;
};

} // namespace tools::intrinsic
