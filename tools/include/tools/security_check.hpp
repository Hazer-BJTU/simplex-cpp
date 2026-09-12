#pragma once

//
// security_check.hpp — the tools module's default invocation security policy
// ==========================================================================
//
// Every invocation passes one gate before it runs: ToolInterface::
// security_check(). A tool may answer that gate itself — it knows things no
// policy can, e.g. "this build never writes" — but the module owes every tool a
// working default, and that default is decided entirely by the InvokeSecurity
// level the call was settled with. write_attributes() (toolsets.hpp) writes
// that level onto the query BEFORE this gate on purpose: the policy then reads
// the tool's own declaration instead of guessing from the tool's name.
//
//   Trusted        pass. The tool declared this call does not need asking.
//                  Nothing else is consulted and no event is published, so a
//                  Trusted call costs nothing even when no host is attached.
//   DefaultDeny    refuse. "Never cleared to run" is the honest reading of a
//                  level the tool never raised, and it is how
//                  model_io::InvokeSecurity behaves elsewhere: DefaultDeny is
//                  first in the enum and an unrecognised level reads back as
//                  it, so the unknown case fails closed.
//   RequireConfirm ASK, by publishing an InvokeConfirmEvent on the async event
//                  bus (utils/async_eventbus) and awaiting the answer. This is
//                  the case the event exists for.
//
// Why an event, and not a callback or a return code. The party that can answer
// "may this run?" is usually not the code that runs the tool: it is the front
// end holding the human — a terminal prompt, a GUI dialog, a policy service
// over the network. The async event bus is the tree's mechanism for reaching
// such a party: the publisher holds no reference to the answerer, a handler may
// live in another dlopened plugin (the bus is a SHARED-library singleton, so
// host and plugins bind ONE bus per process by SONAME), and a handler is a
// coroutine, so a confirmation may take as long as the human takes without
// blocking the thread. Nothing in the tool layer needs to know who — or
// whether anyone — is listening.
//
// Fail closed. RequireConfirm is a request for approval, so anything short of
// an explicit approval refuses the invocation:
//
//   no handler subscribed       refuse — nothing can confirm
//   handler leaves it unanswered refuse — silence is not consent
//   handler throws              refuse — the exception propagates out of this
//                               policy and the toolset reports it at
//                               InvokeException::Stage::SecurityCheck, so a
//                               broken confirmer denies the call and its
//                               message survives into the failure record
//   Approved                    pass
//
// The event is the request AND the answer. publish() FOLDS it through the
// subscribed handlers, each returning a new copy: a handler receives the
// settled query in `query` and answers by writing `decision` (and, if it wants
// its words in the failure record, `reason`); a handler that only observes
// returns its input unchanged. The fold is serial and in registration order, so
// the LAST handler to answer decides — a policy layer subscribed after the UI
// can veto an approval or grant one the UI refused. Handlers that answer
// nothing leave the decision at Unanswered, which is why silence refuses.
//
// The reason strings. On refusal the reason is what the failure record shows
// the model ("security check denied: <reason>"), so the defaults below read as
// clauses after that colon, and a handler's own `reason` is passed through
// verbatim — it is the host's prose and the module has no business editing it.
// On approval the reason is informational: the toolset drops it and runs the
// call.
//
// Overriding. default_security_check() is a free function so it can serve three
// callers: the ToolInterface default in toolsets.hpp, a tool that wants the
// default policy PLUS its own extra condition, and a host that runs its own bus
// instead of the process-wide one. The hook is virtual for tools that need
// neither.
//

#include <string>
#include <tuple>

#include <boost/asio/awaitable.hpp>

#include "dataclass/model_io.hpp"
#include "eventbus/async_event_bus.hpp"

namespace tools {

/**
 * How an invocation that was waiting for confirmation was answered.
 *
 * Unanswered is where every request starts and where it stays when nobody
 * decides — which is what makes "no host attached" and "the host declined to
 * answer" the same, safely refusing, outcome for the caller. A handler answers
 * by moving the decision to Approved or Denied; it may answer a request another
 * handler already answered, and the last answer in registration order is the
 * one the publisher reads.
 */
enum class ConfirmDecision {
    Unanswered, // nobody decided yet (the state a request is published in)
    Approved,   // the invocation may run
    Denied,     // the invocation must not run
};

/**
 * A RequireConfirm invocation asking the host whether it may run — and the
 * host's answer, since the bus folds the event back to the publisher.
 *
 * One event type serves both directions because that is what the async bus's
 * fold gives: the publisher constructs it Unanswered, each handler receives the
 * previous value by const reference and returns a new one with its answer
 * written in, and publish() hands the last one back. A handler therefore sees
 * whatever earlier handlers decided, and may agree, reverse them, or pass the
 * question along.
 *
 * The `query` is the SETTLED call: ensure_arguments() has filled in defaults
 * and write_attributes() has written the type and security level, so a
 * confirmer may show the human the final arguments and may rely on the
 * attributes it is asked to confirm. It is a copy, so a handler that inspects
 * or annotates it cannot disturb the invocation in flight.
 */
struct InvokeConfirmEvent : eventbus::AsyncEventBase {
    /// The settled call awaiting a decision.
    model_io::InvokeQuery query;
    /// The answer; Unanswered for a request nobody has decided yet.
    ConfirmDecision decision = ConfirmDecision::Unanswered;
    /// Why, in the host's own words. Carried into the failure record when the
    /// invocation is refused; ignored when it is approved.
    std::string reason;

    /// The request a publisher sends for `query`: unanswered, no reason yet.
    [[nodiscard]] static InvokeConfirmEvent for_query(
        const model_io::InvokeQuery& query)
    {
        InvokeConfirmEvent request;
        request.query = query;
        return request;
    }
};

/**
 * The default invocation security policy: decide an invocation from its
 * InvokeSecurity level, asking the host (through the async event bus) only for
 * RequireConfirm.
 *
 * @param query the settled call, with the security level write_attributes()
 *        wrote onto it.
 * @param bus the bus a confirmation is published on. Defaults to the
 *        process-wide eventbus::default_async_bus(), which is what lets a host
 *        in another plugin answer; pass your own to keep confirmations inside
 *        a component, or in tests.
 * @return {may_run, reason}: the decision, and why — prose for the failure
 *         record when the decision is false, informational when it is true.
 *
 * Trusted passes and DefaultDeny refuses without touching the bus. A
 * RequireConfirm call is refused unless a subscribed handler moves the decision
 * to Approved: no subscriber, no answer, and a handler that throws all refuse
 * (the last by propagating the exception to the caller, which is where the
 * toolset turns it into a SecurityCheck failure carrying the handler's
 * message).
 *
 * A throwing handler aborts the fold, so the remaining handlers do not run.
 * That is the bus's propagate mode — the default — chosen here because a
 * confirmer that crashed should be reported, not silently treated as silence.
 */
[[nodiscard]] inline boost::asio::awaitable<std::tuple<bool, std::string>>
default_security_check(
    const model_io::InvokeQuery& query,
    eventbus::AsyncEventBus& bus = eventbus::default_async_bus())
{
    switch (query.security) {
        case model_io::InvokeSecurity::Trusted:
            co_return std::make_tuple(
                true,
                std::string("the invocation is trusted and runs without confirmation"));
        case model_io::InvokeSecurity::DefaultDeny:
            co_return std::make_tuple(
                false,
                std::string("the invocation's security level is default deny, "
                            "so it was never cleared to run"));
        case model_io::InvokeSecurity::RequireConfirm:
            break; // the one level that asks; handled below
    }

    // Best-effort wording only: the count picks which "nobody answered" message
    // the host reads, never whether the call runs — both branches refuse. A
    // handler may subscribe between this count and the publish below; that
    // changes the message, not the decision.
    const bool anyone_listening = bus.subscriber_count<InvokeConfirmEvent>() > 0;

    const InvokeConfirmEvent answered =
        co_await bus.publish(InvokeConfirmEvent::for_query(query));

    switch (answered.decision) {
        case ConfirmDecision::Approved:
            co_return std::make_tuple(
                true,
                answered.reason.empty()
                    ? std::string("the confirmation handler approved the invocation")
                    : answered.reason);
        case ConfirmDecision::Denied:
            co_return std::make_tuple(
                false,
                answered.reason.empty()
                    ? std::string("the confirmation handler denied the invocation")
                    : answered.reason);
        case ConfirmDecision::Unanswered:
            co_return std::make_tuple(
                false,
                anyone_listening
                    ? std::string("no subscribed handler answered the "
                                  "confirmation request")
                    : std::string("no handler is subscribed to confirm the "
                                  "invocation"));
    }

    // Unreachable: the switch above covers every ConfirmDecision. Keeps
    // -Wreturn-type quiet, as the stage switches in invoke_exception.hpp do.
    co_return std::make_tuple(
        false, std::string("no handler is subscribed to confirm the invocation"));
}

} // namespace tools
