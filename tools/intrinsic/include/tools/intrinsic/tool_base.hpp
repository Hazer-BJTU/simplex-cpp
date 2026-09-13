#pragma once

//
// tool_base.hpp — what every intrinsic tool shares
// =================================================
//
// The base an intrinsic tool derives from, carrying the four things every one
// of them needs and none of them should re-solve:
//
//   THE INVOCABLE'S STORAGE   get_details() returns a reference by contract,
//                             so the description the model reads has to live
//                             somewhere the tool owns for as long as its set
//                             can hand it out. That is `_details`, filled in
//                             by the derived tool's constructor.
//   ARGUMENT READING          the typed optional/required accessors below.
//                             Every one of them REFUSES rather than coerces,
//                             for the reason given on each.
//   THE JSON RESULT           json_content(): a pretty-printed object in a
//                             text part, which is how every intrinsic tool
//                             answers (see the toolset_base.hpp header for
//                             why that shape and not prose).
//   THE CONFIRMATION'S BUS    security_check() routes the module's default
//                             policy at a bus the tool was given, so a
//                             component (or a test) can keep its
//                             confirmations to itself instead of subscribing
//                             to the process-wide one.
//
// WHAT IS DELIBERATELY NOT HERE. Nothing about any particular domain: no
// session ids, no paths, no handles. A toolset's own base adds those on top —
// process/tools.hpp has ProcessToolBase for its session-id argument and its
// "no such session" failure — and this class stays the part a second toolset
// family can inherit unchanged.
//
// ARGUMENTS ARE CHECKED IN ensure_arguments(), NEVER IN invoke(). That is the
// invocation layer's dependency order (tools/toolsets.hpp): the security check
// and the human confirmation must see the SETTLED arguments, so defaults are
// filled in and types validated before anything judges the call. The
// accessors below are therefore called TWICE for most tools — once in
// ensure_arguments() to validate (discarding the value), once in invoke() to
// read it — and they are pure reads, so that costs nothing but says the
// checking happened at the checkpoint that owns it.
//
// FAILURE IS BY EXCEPTION, AT THE RIGHT CHECKPOINT. bad_argument() raises
// Stage::ArgumentParse, which is the one failure class a model can fix by
// re-reading its own call, so those messages name the offending property and
// say what was expected. A tool whose arguments were fine but whose subject is
// gone raises Stage::Invoke instead — that distinction is worth keeping,
// because a model fixes the first by correcting itself and the second by
// looking at the world again.
//

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

#include <boost/asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include "dataclass/model_io.hpp"
#include "eventbus/async_event_bus.hpp"
#include "tools/security_check.hpp"
#include "tools/toolsets.hpp"

namespace tools::intrinsic {

/**
 * The base every intrinsic tool derives from: its Invocable's storage, the
 * argument accessors, the JSON result shape, and the bus its confirmation is
 * asked on.
 *
 * Domain-neutral on purpose — see the file header for what a toolset's own
 * base adds on top.
 */
class IntrinsicTool : public tools::ToolInterface {
public:
    /**
     * @param bus the bus a RequireConfirm call publishes its
     *        InvokeConfirmEvent on, or nullptr for the process-wide
     *        eventbus::default_async_bus() — which is what a host wants, so
     *        that a confirmer living in another module (a UI, a dlopened
     *        plugin) can answer.
     *
     *        A non-null bus keeps a component's confirmations INSIDE it: the
     *        free-function form of the policy exists for exactly that
     *        (tools/security_check.hpp), and it is what lets a test drive the
     *        gate without subscribing a handler to the process-wide bus, where
     *        it would outlive the test and answer for everyone else.
     *
     *        Borrowed, not owned: the bus must outlive the tool.
     */
    explicit IntrinsicTool(eventbus::AsyncEventBus* bus = nullptr) noexcept;

    const model_io::Invocable& get_details() const noexcept override;

    /// The module's default policy, on THIS tool's bus. Overridden only to
    /// route the question; the decision itself is entirely
    /// default_security_check()'s (Trusted passes, DefaultDeny refuses,
    /// RequireConfirm asks and silence refuses).
    boost::asio::awaitable<std::tuple<bool, std::string>> security_check(
        const model_io::InvokeQuery& query) override;

protected:
    // ---- argument reading ---------------------------------------------------
    //
    // All of these READ the query and nothing else. A missing value yields the
    // fallback; a value of the wrong TYPE is a failure, never a coercion.

    /// The value under `key`, or nullptr when absent or JSON null. Null counts
    /// as absent throughout this module, matching the data contract's rule
    /// (dataclass/model_io.hpp, protocol rules 3+6).
    [[nodiscard]] static const nlohmann::json* find_argument(
        const model_io::InvokeQuery& query, std::string_view key);

    /// A required non-empty string. Throws ArgumentParse naming `key` when it
    /// is missing, not a string, or empty; `what_it_is` completes the message
    /// ("the id a previous spawn_process returned").
    [[nodiscard]] static std::string require_string(
        const model_io::InvokeQuery& query, std::string_view key,
        std::string_view what_it_is);

    /// An optional string with a default. Throws ArgumentParse when present
    /// but not a string.
    [[nodiscard]] static std::string optional_string(
        const model_io::InvokeQuery& query, std::string_view key,
        std::string_view fallback = {});

    /// An optional boolean with a default. Throws ArgumentParse when present
    /// but not a boolean — deliberately no coercion, since a "false" STRING
    /// is truthy under every coercion rule, and a model handed the opposite
    /// of what it asked for has no way to see why.
    [[nodiscard]] static bool optional_bool(const model_io::InvokeQuery& query,
                                            std::string_view key, bool fallback);

    /// An optional non-negative integer with a default.
    ///
    /// Both integer KINDS are accepted and the sign is checked separately:
    /// nlohmann stores a plain positive literal as a SIGNED integer, so
    /// is_number_unsigned() alone would reject every ordinary value while
    /// accepting none. Floats and strings are refused rather than coerced
    /// (truncating 1.9 to 1, or reading "soon" as 0, runs a different call
    /// than the one that was asked for), and a negative value is refused
    /// because it would wrap into an enormous unsigned one — a "timeout" of a
    /// few hundred million years reads as a hang.
    [[nodiscard]] static std::uint64_t optional_uint(
        const model_io::InvokeQuery& query, std::string_view key,
        std::uint64_t fallback);

    /// An optional array of strings, checked element by element so a single
    /// non-string names its own index. Empty when absent.
    [[nodiscard]] static std::vector<std::string> optional_string_list(
        const model_io::InvokeQuery& query, std::string_view key);

    // ---- failures -----------------------------------------------------------

    /// Refuse the call's ARGUMENTS: Stage::ArgumentParse, the one failure a
    /// model can fix by re-reading what it sent.
    [[noreturn]] static void bad_argument(std::string message);

    /// Refuse the call's SUBJECT: Stage::Invoke — the arguments were correct,
    /// the world moved on. A model fixes this by looking again, not by
    /// correcting itself.
    [[noreturn]] static void invoke_failed(std::string message);

    // ---- results ------------------------------------------------------------

    /// The result every intrinsic tool answers with: the JSON object as a
    /// text part, indented (a model reads it, and so does a human reading the
    /// transcript over its shoulder).
    [[nodiscard]] static model_io::Content json_content(nlohmann::json payload);

    // ---- schema helpers -----------------------------------------------------
    //
    // Small builders for the JSON Schema a tool advertises. Here rather than
    // in each toolset because the shapes are the same everywhere, and a
    // description on every property is the whole contract a model works from.

    [[nodiscard]] static nlohmann::json string_property(
        std::string_view description);
    [[nodiscard]] static nlohmann::json bool_property(
        std::string_view description, bool fallback);
    [[nodiscard]] static nlohmann::json uint_property(
        std::string_view description, std::uint64_t fallback);
    [[nodiscard]] static nlohmann::json string_list_property(
        std::string_view description);
    [[nodiscard]] static nlohmann::json enum_property(
        std::string_view description, std::vector<std::string> values,
        std::string_view fallback);

    /// An object schema from its properties and the subset that is required —
    /// the `{"type": "object", "properties": {...}, "required": [...]}` shape
    /// every tool's argument_schema has.
    [[nodiscard]] static nlohmann::json object_schema(
        nlohmann::json properties, std::vector<std::string> required = {});

    /// The tool as the model sees it. Filled in by the derived constructor;
    /// get_details() hands out a reference to it.
    model_io::Invocable _details;

private:
    /// nullptr => the process-wide bus, resolved at check time rather than
    /// captured here: default_async_bus() is a shared-library singleton, and
    /// binding it in a constructor would tie a tool to whichever module built
    /// it.
    eventbus::AsyncEventBus* _bus = nullptr;
};

} // namespace tools::intrinsic
