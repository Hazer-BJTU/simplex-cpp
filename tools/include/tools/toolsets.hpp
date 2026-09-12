#pragma once

//
// toolsets.hpp — the tool side of the contract: what one tool is, and how a set
// of them answers one InvokeQuery
// ==========================================================================
//
// Two types, two responsibilities:
//
//   ToolInterface  ONE tool. get_details() is how the model finds it (an
//                  Invocable: name, description, argument schema); invoke()
//                  runs it; the hooks around invoke() are the checkpoints
//                  invoke_exception.hpp names, each with a usable default so a
//                  tool implements only what it must — invoke() itself is the
//                  exception: a tool that does not implement it refuses at the
//                  Invoke checkpoint rather than answering the model with an
//                  empty result.
//
//   ToolSet        The registry a host routes calls to. dispatch() resolves a
//                  call's tool name to a tool; handle() runs that call through
//                  every checkpoint and ALWAYS returns an InvokeReturn — the
//                  success or the failure record for that call. It never lets
//                  a failure escape, because its caller is a detached agent
//                  loop: an invocation outlives the frame that started it, so
//                  there is no caller left to throw at (invoke_exception.hpp).
//
// The checkpoint sequence handle() runs, and the stage each failure is reported
// at — the same list, in the same order, as InvokeException::Stage:
//
//   Dispatch       dispatch() resolved no tool, or threw while resolving one
//   ArgumentParse  ensure_arguments() or write_attributes() failed: the call
//                  could not be settled into a runnable query
//   SecurityCheck  security_check() refused, or threw
//   Invoke         invoke() threw
//   ResultCheck    check_result() threw: the tool ran, but its output broke
//                  the tool's own contract
//
// The order is a dependency order. Ensuring the arguments is side-effect free,
// so it runs first, and everything that reads the settled query follows it: the
// attributes this layer writes onto it, the security check that judges it, the
// invocation itself, and the result check that wraps what came back. A policy
// decision taken on half-settled arguments would be a decision about a
// different call — see the longer argument in invoke_exception.hpp.
//
// Every failure travels back as an ordinary tool result: the model reads the
// rendering in output.raw, the host classifies the marker in extras (is_error /
// error_stage), and the query travels in the record so the failure correlates
// to the call by query.id. handle() upholds that even for a tool that raised
// its own InvokeException with no query attached.
//
// What this layer deliberately does not do: schedule. InvokeType (read-only /
// parallel write / serial write) is written onto the query for the caller that
// runs several invocations, and handle() runs exactly the one call it was
// given.
//

#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include "dataclass/model_io.hpp"
#include "tools/invoke_exception.hpp"
#include "tools/security_check.hpp"

namespace tools {

/**
 * One tool: the description the model sees, the call the host routes to it, and
 * the hooks that decide whether that call may run at all.
 *
 * Every hook has a default, so a tool implements only what it needs: a
 * read-only tool that trusts its arguments overrides invoke() and, at most,
 * write_attributes() to say what it is.
 *
 * Failure is by exception. A hook that cannot do its job throws an
 * InvokeException for a failure one of the checkpoints names (that type is how
 * a hook reports WHICH checkpoint failed, and the failure record is built from
 * it), or any std::exception for one it does not — the toolset reports that at
 * the stage it was running when it escaped. A hook that merely refuses, e.g.
 * ensure_arguments() finding a missing required field, throws as well; nothing
 * here returns a status code.
 *
 * Threading. The hooks marked "no write to inner states" are const and are
 * required not to touch the tool's mutable state: they describe or judge a
 * call, and the same tool instance may have other invocations in flight.
 * invoke() is the one hook that may reach into the tool's own state, and
 * security_check() may too, e.g. to ask a connection it owns.
 */
class ToolInterface {
public:
    virtual ~ToolInterface() = default;

    /// The tool as the model sees it. Returned by reference: the tool owns the
    /// storage and must keep it alive as long as its toolset can hand the tool
    /// out, since callers (a request builder flattening the tool list) read it
    /// without copying.
    virtual const model_io::Invocable& get_details() const noexcept = 0;

    /// The tool's own lifecycle, owned by the toolset that constructed it:
    /// build() acquires whatever invoke() needs (a file map, a connection, a
    /// compiled pattern), release() gives it back. False refuses the tool — the
    /// toolset does not hand it out.
    ///
    /// Neither hook takes a query, so neither is part of the per-call checkpoint
    /// sequence: a toolset calls build() once when it registers the tool and
    /// release() when it drops it. handle() calls neither.
    virtual bool build() noexcept { return true; }

    /// Give back what build() acquired. Called once, when the tool is dropped.
    virtual void release() noexcept {}

    /**
     * Settle the call's arguments: check what the tool requires and fill in
     * defaults, so that everything downstream reads complete arguments.
     *
     * Side-effect free on purpose — it reads and completes `query`, and touches
     * nothing else. That is precisely why it runs before the security check and
     * the attribute writing: those two read the settled arguments, and a
     * refusal or a confirmation raised on half-settled arguments would be about
     * a different call than the one that would run.
     *
     * Throws when the arguments do not satisfy the tool's contract: an
     * InvokeException at InvokeException::Stage::ArgumentParse carrying a
     * message the model can act on, or any std::exception, which the toolset
     * reports at that same stage. Both reach the model as the tool result for
     * this call.
     *
     * The default requires nothing: a tool whose arguments are all optional
     * needs no check.
     */
    virtual void ensure_arguments(model_io::InvokeQuery& /* query */) const {}

    /**
     * Write the invocation attributes the model request leaves indeterminate
     * onto the settled query: how the call touches state (InvokeType) and how
     * much trust it needs (InvokeSecurity). The model names a tool and its
     * arguments; only the tool knows whether that reads or writes, and whether
     * running it unattended is acceptable.
     *
     * Runs after ensure_arguments for the same reason the security check does:
     * what it writes may depend on the settled arguments — a write only counts
     * as one when the path argument says so.
     *
     * The default is the cautious pair. SerialWrite, because a tool that
     * forgets to declare itself must not be advertised as side-effect free
     * (ReadOnly is what lets a caller run invocations in parallel) — a wrong
     * ReadOnly is a data race, a wrong SerialWrite is merely slower. And
     * RequireConfirm, so a tool that never says how much it can be trusted is
     * judged by default_security_check() (tools/security_check.hpp): asked
     * about, and refused unless someone answers. A tool that wants Trusted —
     * or wants to be refused outright with DefaultDeny — says so here.
     *
     * Throws like ensure_arguments(), and is reported at the same
     * InvokeException::Stage::ArgumentParse: it is the same phase, settling the
     * query before any check runs.
     */
    virtual void write_attributes(model_io::InvokeQuery& query) const
    {
        query.type = model_io::InvokeType::SerialWrite;
        query.security = model_io::InvokeSecurity::RequireConfirm;
    }

    /**
     * Decide whether this invocation may run, on the settled query — the last
     * gate before invoke().
     *
     * @param query the settled call: arguments ensured, type and security
     *        written by write_attributes().
     * @return {may_run, reason}. The reason is prose: when may_run is false the
     *         toolset puts it in the failure record the model reads, and when
     *         it is true the toolset drops it, so it is worth a sentence only
     *         when it explains a refusal.
     *
     * The default is the module's whole policy: model_io::InvokeSecurity::
     * Trusted runs, DefaultDeny refuses, and RequireConfirm asks the host
     * through an InvokeConfirmEvent on the async event bus — refusing if nobody
     * answers. Override this when the tool knows something the policy cannot
     * (e.g. "this build has no write path, always allow") or when the answer
     * comes from a private bus instead of the process-wide one; a tool that
     * wants the default policy PLUS one extra condition calls
     * default_security_check() from its override.
     *
     * Asynchronous because a real answer may be: a dialog waits for a human, a
     * policy service waits for the network. It is not const, because asking may
     * use the tool's own state (a connection, a cache of decisions).
     *
     * Throws to refuse with machinery rather than a decision — a broken
     * confirmer's exception propagates from default_security_check() — and the
     * toolset reports it at InvokeException::Stage::SecurityCheck.
     */
    virtual boost::asio::awaitable<std::tuple<bool, std::string>> security_check(
        const model_io::InvokeQuery& query)
    {
        co_return co_await default_security_check(query);
    }

    /**
     * Run the call and produce its output.
     *
     * @param query the settled, checked call. The arguments are final by now.
     * @return the output part for this call; check_result() wraps it into the
     *         InvokeReturn the conversation carries.
     *
     * Throws on failure — an InvokeException at InvokeException::Stage::Invoke,
     * or any std::exception the toolset reports at that stage.
     *
     * The default REFUSES at that same checkpoint: a tool that does not
     * implement this has nothing to run, and answering with an empty text part
     * would hand the model something that reads like a result. The failure
     * names the tool, so the message says which hook is missing. A tool is
     * expected to override this; leaving it is a bug in the tool, reported as
     * one.
     */
    virtual boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& /* query */)
    {
        throw InvokeException(
            InvokeException::Stage::Invoke,
            std::format("tool \"{}\" does not implement invoke()",
                        get_details().name));
        // Unreachable, like the trailing returns in invoke_exception.hpp's
        // stage switches: a coroutine needs a co_ keyword to BE a coroutine,
        // and this one must throw when awaited rather than at the call.
        co_return model_io::Content{};
    }

    /**
     * Turn the tool's output into the record the conversation carries: the call
     * it answers, the output, and whatever extras belong on the result.
     *
     * The last checkpoint, and the only hook that sees both halves: a tool whose
     * output has a contract of its own — "this is JSON, and it parses", "this
     * path is inside the workspace" — validates it here and throws
     * InvokeException{Stage::ResultCheck} rather than handing the model
     * something it cannot use. A tool with no contract of its own keeps the
     * default, which simply wraps.
     *
     * Takes both by value: it owns its answer, and the query it embeds is the
     * settled one the caller passed. Like the other "no write to inner states"
     * hooks it is const.
     */
    virtual model_io::InvokeReturn check_result(
        model_io::InvokeQuery query, model_io::Content output) const
    {
        return model_io::InvokeReturn{
            .query = std::move(query),
            .output = std::move(output),
            .extras = {}
        };
    }
};

/**
 * A set of tools, as one routable unit: what it offers, how a call finds its
 * tool, and how that call is run end to end.
 *
 * A host holds tool sets rather than tools, because the set is the unit that
 * knows how to resolve a name (dispatch) and what to do with a call whose name
 * it does not know (return the Dispatch failure record). A set is free to be
 * heterogeneous — the tools in it may share nothing but the interface — and
 * free to be one tool, e.g. a shim that forwards to a remote provider.
 */
class ToolSet {
public:
    using ToolHandle = std::shared_ptr<ToolInterface>;

    virtual ~ToolSet() = default;

    /// The set's name, as the failure records and the logs spell it. Returned
    /// by reference-like view, so the set owns the storage.
    virtual std::string_view name() const noexcept = 0;

    /// Every tool this set offers, in the order it wants them presented to the
    /// model. Returns by value (the caller flattens a catalogue it does not
    /// own), which is why this is not noexcept: building the vector allocates.
    virtual std::vector<model_io::Invocable> get_tools() const = 0;

    /// Resolve `query` to the tool that handles it, or nullptr when this set has
    /// no such tool — the case handle() reports as a Dispatch failure, which is
    /// why the contract allows returning nullptr instead of throwing.
    ///
    /// Not noexcept: a set that resolves through a map, a plugin registry or a
    /// remote catalogue may fail while doing so, and handle() reports such a
    /// failure as a Dispatch-stage record for the model rather than letting it
    /// terminate the process.
    virtual ToolHandle dispatch(const model_io::InvokeQuery& query) const = 0;

    /// The names get_tools() advertises, for diagnostics and for a host that
    /// routes by name across several sets. Allocates, so not noexcept.
    [[nodiscard]] std::vector<std::string> supported_names() const
    {
        std::vector<std::string> results = {};
        for (const auto& tool : get_tools()) {
            results.push_back(tool.name);
        }
        return results;
    }

    /**
     * Run one call to completion and return its record: the tool's result, or
     * the failure record standing in for it. Never throws, and never returns an
     * empty record — every path correlates to the caller's query by query.id.
     *
     * @param query the call, taken by value: handle() settles it (arguments
     *        ensured, attributes written) and the record carries the settled
     *        form.
     *
     * The checkpoint sequence is the one the file header lists. Each step
     * records the stage it is about to run in `stage`, so a failure that
     * arrives as a plain exception is still reported at the right checkpoint.
     *
     * Two deliberate details of the failure paths:
     *
     *  - check_result() gets a COPY of the query. It takes the query by value,
     *    so moving ours into it would leave the catch blocks holding a
     *    moved-from query, and a failure record without query.id no longer
     *    correlates to the model's call — the one thing the record is for.
     *  - An InvokeException a tool raised with no query of its own is rebuilt
     *    around ours, for the same reason: a tool may throw
     *    InvokeException{Stage::Invoke, "read failed"} and know nothing about
     *    the call it was invoked with, but the record still has to answer that
     *    call. A query the tool DID carry wins — it may be the one it actually
     *    failed on.
     */
    virtual boost::asio::awaitable<model_io::InvokeReturn> handle(
        model_io::InvokeQuery query)
    {
        // The stage every failure is reported at until a later step claims it.
        // Dispatch is accurate for everything that happens from here until
        // ensure_arguments() starts: resolving the tool is the first checkpoint.
        InvokeException::Stage stage = InvokeException::Stage::Dispatch;
        try {
            ToolHandle tool_handle = dispatch(query);
            if (tool_handle == nullptr) {
                // The message is rendered BEFORE the throw, never inline: the
                // arguments of a call are evaluated in an unspecified order, so
                // a std::format(...) reading query.name in the same argument
                // list as std::move(query) may read the moved-from string and
                // name no tool at all. (The test suite caught exactly that.)
                const std::string message = std::format(
                    "no tool named \"{}\" in toolset \"{}\"", query.name, name());
                throw InvokeException(InvokeException::Stage::Dispatch, message,
                                      std::move(query), {});
            }

            stage = InvokeException::Stage::ArgumentParse;
            tool_handle->ensure_arguments(query);
            tool_handle->write_attributes(query);

            stage = InvokeException::Stage::SecurityCheck;
            auto [passed, reason] = co_await tool_handle->security_check(query);
            if (!passed) {
                // Rendered first, for the reason above.
                const std::string message =
                    std::format("security check denied: {}", reason);
                throw InvokeException(InvokeException::Stage::SecurityCheck,
                                      message, std::move(query), {});
            }

            stage = InvokeException::Stage::Invoke;
            model_io::Content content = co_await tool_handle->invoke(query);

            stage = InvokeException::Stage::ResultCheck;
            // A copy, not std::move(query): see the note above. check_result()
            // owns what it is given and embeds the settled query in the record.
            co_return tool_handle->check_result(query, std::move(content));
        } catch (const InvokeException& failure) {
            if (failure.query().id.empty() && !query.id.empty()) {
                co_return InvokeException(
                    failure.stage(), failure.message(), query,
                    failure.error_code()).to_invoke_return();
            }
            co_return failure.to_invoke_return();
        } catch (const std::exception& e) {
            // A plain exception from a hook, or from anything a hook called:
            // reported at the checkpoint that was running.
            co_return InvokeException(stage, e.what(), std::move(query), {})
                .to_invoke_return();
        } catch (...) {
            // Same, for a throw that is not a std::exception at all.
            co_return InvokeException(
                stage, "an unknown error, not a std::exception",
                std::move(query), {}).to_invoke_return();
        }
    }
};

} // namespace tools
