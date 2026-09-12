#pragma once

//
// invoke_exception.hpp — the tools module's tool-invocation exception
// ===================================================================
//
// One failure type for every checkpoint of a tool invocation, plus the bridge
// that turns it into the record the conversation carries: a model_io
// InvokeReturn.
//
// Why the bridge exists. A tool call is not always awaited by whoever started
// it: the agent loop dispatches an invocation and moves on, so when the tool
// fails there may be no caller left to throw at. A failure then has to travel
// back as an ordinary tool result — one InvokeReturn in the conversation,
// correlated to the call by query.id — and the model has to be able to read it
// as prose. Hence the two conversions on the exception:
//
//   - to_string()/what(): the one-line rendering. It is what lands in
//     InvokeReturn::output.raw — the text the model reads.
//   - to_invoke_return(): that text PLUS the machine-readable failure marker
//     in InvokeReturn::extras, so a host can tell a failed invocation from a
//     successful one that happens to return similar-looking text, without
//     parsing prose. operator InvokeReturn() makes that conversion implicit,
//     so the unified handler is a plain `catch (const InvokeException& e) {
//     co_return e; }` in the coroutine that produces the result.
//
// The marker is extras_key -> {"stage": <stage_key()>, "message": <bare
// message>}; is_error() / error_stage() read it back. The stage survives as
// its snake_case key, so a restored session classifies a failure exactly the
// way the live one did.
//
// Shape. As in the other module exceptions of this tree (endpoint's
// HttpRequestException, which process::ProcessException copies, and intercom's
// WsException), the class carries one Stage, an optional error_code, and the
// context that identifies the failing operation — and what() ALREADY IS the
// full one-line rendering, not the bare message. That equivalence is
// deliberate: a host that catches this type only as std::exception — the one
// dependable catch across a dlopen boundary, where each module carries its own
// typeinfo copy — still sees the whole context in what(). What differs here is
// the staging, cut along the five checkpoints a tool call passes (see Stage),
// and the InvokeReturn bridge, which no transport-level failure needs.

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <boost/system/error_code.hpp>

#include "dataclass/model_io.hpp"

namespace tools {

/**
 * Describes a failure in one stage of a tool invocation and retains the
 * InvokeQuery that failed, so the failure can be correlated back to the call
 * the model made (query().id) and reported as that call's result.
 *
 * The query is held by value, not by reference: the exception is typically
 * raised inside a detached invocation and consumed later, in a handler whose
 * own frame — and the caller's — may already be gone.
 */
class InvokeException : public std::runtime_error {
public:
    /// The checkpoint an invocation failed at, in the order the checkpoints
    /// run. A handler can act on the stage: Dispatch means the call never
    /// reached a tool (nothing was resolved to run, and the model can only fix
    /// it by naming a tool that exists), ArgumentParse is worth returning to
    /// the model to correct, SecurityCheck is a refusal (not retryable, and
    /// not a mistake the model should be asked to fix), Invoke may be worth
    /// retrying, and ResultCheck means the tool ran but its output broke its
    /// own contract.
    ///
    /// That order is a dependency order, not merely a sequence. Ensuring the
    /// arguments (the tool's ensure_arguments step) is side-effect free — it
    /// only reads and completes the query — so it comes first, and the steps
    /// that consume the ensured arguments follow it: the attributes written
    /// onto the query (write_attributes) and the security check
    /// (security_check), which may both depend on the values it filled in. A
    /// refusal or a retry raised on arguments that were never settled would
    /// not mean what its handler takes it to mean.
    enum class Stage {
        Dispatch,      // the call could not be dispatched to a tool (e.g. no such tool)
        ArgumentParse, // the arguments did not satisfy the tool's contract
        SecurityCheck, // refused by the invocation's security policy
        Invoke,        // the tool itself failed while running
        ResultCheck,   // the result did not satisfy the tool's contract
        Unknown        // failed outside those checkpoints (an unclassified throw)
    };

    InvokeException(
        Stage stage,
        std::string message,
        model_io::InvokeQuery query = {},
        boost::system::error_code ec = {})
        : std::runtime_error(render(stage, message, ec, query)),
          stage_(stage),
          message_(std::move(message)),
          query_(std::move(query)),
          ec_(ec)
    {}

    [[nodiscard]] Stage stage() const noexcept { return stage_; }
    /** The bare message, without the stage phrase and context that what()
     *  wraps around it. This is the text the failure marker carries. */
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    /** The invocation that failed; an id-carrying query correlates the failure
     *  to the model's tool call. */
    [[nodiscard]] const model_io::InvokeQuery& query() const noexcept
    {
        return query_;
    }
    /** The error code behind the failure when one exists (the transport or
     *  tool error that was translated at this boundary); clear otherwise. */
    [[nodiscard]] const boost::system::error_code& error_code() const noexcept
    {
        return ec_;
    }

    /** The stage's phrase as rendered by to_string(): "while parsing the
     *  invocation arguments", … */
    [[nodiscard]] static constexpr std::string_view stage_phrase(Stage stage) noexcept
    {
        switch (stage) {
            case Stage::Dispatch:      return "while dispatching the invocation to a tool";
            case Stage::ArgumentParse: return "while parsing the invocation arguments";
            case Stage::SecurityCheck: return "while validating the invocation's security";
            case Stage::Invoke:        return "while invoking the tool";
            case Stage::ResultCheck:   return "while validating the tool result";
            case Stage::Unknown:       return "at an unknown stage";
        }
        return "at an unknown stage"; // unreachable; keeps -Wreturn-type quiet
    }

    /** The stage's stable token, as the failure marker spells it
     *  ("security_check", …) — the snake_case counterpart of stage_phrase(),
     *  for machine consumption rather than prose. */
    [[nodiscard]] static constexpr std::string_view stage_key(Stage stage) noexcept
    {
        switch (stage) {
            case Stage::Dispatch:      return "dispatch";
            case Stage::ArgumentParse: return "argument_parse";
            case Stage::SecurityCheck: return "security_check";
            case Stage::Invoke:        return "invoke";
            case Stage::ResultCheck:   return "result_check";
            case Stage::Unknown:       return "unknown";
        }
        return "unknown"; // unreachable; keeps -Wreturn-type quiet
    }

    /** The stage a stage_key() token names, or nullopt for a token this build
     *  does not know (a marker written by a newer producer). */
    [[nodiscard]] static constexpr std::optional<Stage> stage_from_key(
        std::string_view key) noexcept
    {
        if (key == stage_key(Stage::Dispatch)) return Stage::Dispatch;
        if (key == stage_key(Stage::ArgumentParse)) return Stage::ArgumentParse;
        if (key == stage_key(Stage::SecurityCheck)) return Stage::SecurityCheck;
        if (key == stage_key(Stage::Invoke)) return Stage::Invoke;
        if (key == stage_key(Stage::ResultCheck)) return Stage::ResultCheck;
        if (key == stage_key(Stage::Unknown)) return Stage::Unknown;
        return std::nullopt;
    }

    /**
     * One-line, log-friendly rendering of the failure in prose rather than
     * key=value tags: the stage phrase and the message lead, and whichever
     * context fields are set gather into one parenthetical, e.g.
     *   Failed while parsing the invocation arguments: missing required \
     * property "path" (tool read_file; call call_1)
     * Absent fields (no error code, no tool name, no call id) are omitted.
     *
     * Identical to what(): the rendering is baked in at construction (see the
     * class doc for why what() must carry it). This is also the text the model
     * reads in the resulting tool result.
     */
    [[nodiscard]] std::string to_string() const { return what(); }

    /**
     * The failure as the tool result it stands for: the query the invocation
     * was made with, the rendering in output.raw (prose, for the model), and
     * the failure marker in extras (machine-readable, for the host).
     *
     * The error code, when set, is deliberately NOT duplicated into the
     * marker: it is already in error_code() and in the rendered text, and the
     * marker stays a small, stable pair of fields a host can rely on.
     */
    [[nodiscard]] model_io::InvokeReturn to_invoke_return() const
    {
        model_io::InvokeReturn record;
        record.query = query_;
        record.output.type = model_io::ContentType::Text;
        record.output.raw = what();
        record.extras = nlohmann::json::object();
        (*record.extras)[std::string(extras_key)] = nlohmann::json{
            {"stage", std::string(stage_key(stage_))},
            {"message", message_},
        };
        return record;
    }

    /**
     * The same conversion as to_invoke_return(), implicit on purpose: it is
     * what lets a handler that has nothing left to throw at return the failure
     * straight out of an awaitable<InvokeReturn> (`co_return failure;`).
     */
    operator model_io::InvokeReturn() const { return to_invoke_return(); }

    /** InvokeReturn::extras key under which the failure marker lives. */
    static constexpr std::string_view extras_key = "error";

private:
    /// The one-line rendering baked into the runtime_error base at
    /// construction, so what() carries the full context everywhere (the format
    /// is the one to_string() documents).
    static std::string render(
        Stage stage,
        const std::string& message,
        const boost::system::error_code& ec,
        const model_io::InvokeQuery& query)
    {
        std::string rendered = "Failed ";
        rendered += stage_phrase(stage);
        rendered += ": ";
        rendered += message;

        // Context pieces join into a single parenthetical, "; "-separated.
        bool opened = false;
        auto append = [&](std::string_view piece) {
            rendered += opened ? "; " : " (";
            opened = true;
            rendered += piece;
        };
        if (ec) append(ec.message());
        if (!query.name.empty()) append("tool " + query.name);
        if (!query.id.empty()) append("call " + query.id);
        if (opened) rendered += ')';
        return rendered;
    }

    Stage stage_;
    std::string message_;
    model_io::InvokeQuery query_;
    boost::system::error_code ec_;
};

namespace detail {

/// The failure marker inside a record's extras, or nullptr when there is none.
/// A marker that is present but is not an object counts as absent: a producer
/// that overwrote the key with something else is not claiming a failure.
[[nodiscard]] inline const nlohmann::json* error_marker(
    const model_io::InvokeReturn& record) noexcept
{
    if (!record.extras || !record.extras->is_object()) return nullptr;
    const auto it = record.extras->find(InvokeException::extras_key);
    if (it == record.extras->end() || !it->is_object()) return nullptr;
    return &*it;
}

} // namespace detail

/**
 * Whether this tool result is a failure record — i.e. whether it was produced
 * from an InvokeException (directly, or by a catch-all handler that built one
 * for the occasion).
 *
 * The marked record is otherwise an ordinary InvokeReturn: output.raw holds
 * the rendered failure for the model, and the marker in extras is what the
 * host reads to classify it.
 */
[[nodiscard]] inline bool is_error(const model_io::InvokeReturn& record) noexcept
{
    return detail::error_marker(record) != nullptr;
}

/**
 * The stage a failure record was raised at, or nullopt when the record is not
 * a failure record, or its stage token is one this build does not know.
 *
 * The stage needs a reader because it round-trips as a token; the marker's
 * message needs none — it is the bare string under "message".
 */
[[nodiscard]] inline std::optional<InvokeException::Stage> error_stage(
    const model_io::InvokeReturn& record) noexcept
{
    const nlohmann::json* marker = detail::error_marker(record);
    if (marker == nullptr) return std::nullopt;
    const auto it = marker->find("stage");
    if (it == marker->end() || !it->is_string()) return std::nullopt;
    return InvokeException::stage_from_key(it->get_ref<const std::string&>());
}

} // namespace tools
