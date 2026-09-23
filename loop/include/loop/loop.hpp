#pragma once

#include "loop/contracts.hpp"

#include <boost/asio/awaitable.hpp>
#include <stop_token>

namespace loop {

/**
 * Runs one input or continuation through model exchanges and tool batches.
 *
 * @param model Performs converse() and provider-specific history integration.
 *        Its asynchronous converse() implementation must honor Asio terminal
 *        cancellation and join its own work before completing cancellation.
 * @param registry Routes tools and joins each batch. Configuration must remain
 *        fixed during a run; dispatched batches are not interrupted by stop.
 * @param events Explicit synchronous event bus. Writable hooks edit candidates
 *        or the live state as documented in events.hpp. Callbacks must not
 *        reenter run() or retain event references. Serialize subscription
 *        changes with loop execution for predictable editing boundaries.
 * @param executor Executor for model exchanges and parallel tool branches.
 *        Its execution context must keep running until run() completes.
 * @param state Sole persistent conversation and recovery object. The host must
 *        keep it alive and prevent concurrent access until the coroutine exits.
 * @param has_message True starts a user turn from message; false continues the
 *        last turn and ignores message. An empty history cannot be continued.
 * @param message Owned input candidate, edited by BeforeInput when has_message
 *        is true. Must have type UserInput without invokes/invoke_return fields.
 *        Pass an empty MessageItem when continuing an existing turn.
 * @param options Positive model-exchange budget for this invocation.
 * @param stop Thread-safe stop token. While converse() is suspended, a request
 *        emits terminal cancellation on that exchange's serialized executor.
 *        The loop waits for the exchange to exit; it never detaches it. A tool
 *        batch already started is joined and its results committed before exit.
 * @return Completion, cancellation, budget exhaustion, or failure summary.
 *
 * Only one invocation is active per process, including hooks and drain work.
 * Model, registry, events and state are borrowed until completion. Tool results
 * are retained in state.loop before atomic projection, so projection failure
 * can be recovered without repeating tools. Partial model responses are not
 * integrated when converse() ends with cancellation.
 *
 * Admission rejects Tools/Blocked with RecoveryRequired without changing the
 * state; the host decides how to inspect uncertain tool effects. Other ordinary
 * failures return Failed with a diagnostic. A run admitted before failure
 * preserves completed work and publishes its finalized outcome once.
 * EditOnRunFinished may change that outcome; RunFinished is read-only and its
 * subscribers cannot change the final status or error. See events.hpp for the
 * editing contract and README.md for the recovery and cancellation details.
 *
 * This is not a noexcept boundary. Argument/frame construction, coroutine setup,
 * and secondary failures while allocating diagnostics, saving the terminal
 * state or logging may still propagate to the caller. A returned Failed result
 * does not imply that prior commits or external tool effects were undone.
 *
 * Inherited cancellation of the outer coroutine is shielded to protect tool
 * batches: use the explicit stop token.
 * Stopping the executor or destroying dependencies is not safe cancellation.
 * A provider that ignores cancellation cannot be forcibly interrupted safely.
 */
boost::asio::awaitable<RunResult> run(
    llm::LLMModel& model,
    const tools::ToolRegistry& registry,
    eventbus::EventBus& events,
    boost::asio::any_io_executor executor,
    model_io::AgentInputState& state,
    bool has_message,
    model_io::MessageItem message,
    Options options = {},
    std::stop_token stop = {});

} // namespace loop
