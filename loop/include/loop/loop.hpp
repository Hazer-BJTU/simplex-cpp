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
 * @param events Explicit synchronous event bus. Callbacks may edit candidates
 *        at writable hooks, but must not reenter run() or retain event references.
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
 * Exception handling:
 * - The model-wait catch recognizes cancellation only when stop is requested
 *   AND converse() throws boost::system::system_error(operation_aborted).
 *   HTTP retry exhaustion, provider errors and other exceptions instead reach
 *   the outer run-body catch, even if a stop request arrives at the same time.
 * - The outer catch converts validation, recovery, model, integration, registry
 *   and hook exceptions into Failed. std::exception::what() supplies the
 *   diagnostic; other exception types use "unknown exception". The loop does
 *   not retry converse() itself; transport retry belongs to the model adapter.
 * - Before admission, failure is reported in RunResult and logging without a
 *   new progress record or RunFinished event. Recovery may already have
 *   committed previously buffered results before admission is reached.
 * - After admission, failures preserve committed history. Model becomes Ready;
 *   unsettled Tools becomes Blocked; Projection retains pending_results for
 *   recovery. The terminal status/error are saved before RunFinished is sent.
 * - RunFinished exceptions change the outcome to Failed and append a diagnostic.
 *   The event is not sent again, and committed data is not rolled back.
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
