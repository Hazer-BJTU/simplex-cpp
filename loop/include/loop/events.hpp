#pragma once

#include "loop/contracts.hpp"

namespace loop {

// Every reference below is borrowed for synchronous dispatch only. Subscribers
// must not retain it, mutate state through another alias, or reenter run().
// BeforeInput/BeforeModel expose candidates. Full-state editing events below
// edit the live object with one event-wide rollback snapshot when subscribed.

/** Notifies observers after admission and before accepting a new input. */
struct RunStarted {
    const model_io::AgentInputState& state;
};

/**
 * Lets subscribers edit the private input candidate in registration order.
 * The loop validates the final message before integrating it. A throw or stop
 * before integration leaves the conversation without this new input.
 */
struct BeforeInput {
    model_io::MessageItem& input;
};

/** Observes the conversation after the new input has been committed. */
struct InputCommitted {
    const model_io::AgentInputState& state;
};

/**
 * Editable model context copied from the current conversation for one exchange.
 *
 * Execution history and recovery metadata are deliberately excluded. The loop
 * checks that the prompt renders, then commits the candidate after all hooks
 * return. The host remains responsible for tool catalogue and provider-option
 * semantics; these changes also become part of the persistent conversation.
 */
struct ModelContext {
    model_io::PromptTemplate system_prompt;
    std::vector<model_io::Invocable> tools;
    std::optional<nlohmann::json> extras;
};

/**
 * Runs before a model request, exposing the old state and an editable candidate.
 * Later subscribers see earlier subscribers' edits to context. If a subscriber
 * throws, none of this candidate is committed and no request is sent.
 */
struct BeforeModel {
    const model_io::AgentInputState& state;
    ModelContext& context;
};

/**
 * Observes a committed model response before any requested tools are dispatched.
 * If a subscriber throws, outstanding calls receive non-execution results before
 * the run reports failure; the response itself is not rolled back.
 */
struct ModelCommitted {
    const model_io::AgentInputState& state;
};

/**
 * Observes the original model call list immediately before batch dispatch.
 * Subscribers can request a stop through the host's stop_source. Calls cannot
 * be rewritten here because their identity is already in the conversation.
 */
struct BeforeToolBatch {
    const std::vector<model_io::InvokeQuery>& calls;
};

/**
 * Observes a fully projected batch, including batches skipped on a stop request.
 * A subscriber failure cannot roll back these results or the tools' effects.
 * Recovery projection at entry does not publish this event; RunStarted observes
 * the repaired state once the new invocation is admitted.
 */
struct ToolResultsCommitted {
    const model_io::AgentInputState& state;
};

/**
 * Read-only recovery checkpoint after Tools is committed, before dispatch.
 * A throw prevents dispatch and leaves a conservative Blocked recovery marker.
 */
struct ToolDispatchCheckpoint {
    const model_io::AgentInputState& state;
};

/** Read-only checkpoint after complete results enter Projection. A throw keeps
 * the buffer intact for recovery; no tool is replayed on a later invocation. */
struct ToolResultsCheckpoint {
    const model_io::AgentInputState& state;
};

/** Read-only step boundary after all step edits pass validation. A throw fails
 * the run without undoing the validated edits or any tool side effects. */
struct StepFinished {
    const model_io::AgentInputState& state;
};

/**
 * Edits the live state after ToolResultsCommitted subscribers have returned.
 *
 * Useful for pruning complete tool call/result pairs, summarizing old turns or
 * refreshing host metadata before the next model exchange. Also runs for a
 * skipped batch and at the exchange limit, but not for entry-time recovery or
 * a response without tool calls. An earlier observer failure prevents this event.
 *
 * Full-state edit contract (also applies to EditOnRunFinished):
 * - state is the caller's actual object, not a copied event payload. All slots
 *   run synchronously in order and see earlier edits. No references may escape,
 *   no asynchronous work may access state, and run() must not be reentered.
 * - With subscribers, loop takes one full backup before dispatch. A throw or
 *   failed final validation restores it by noexcept move assignment and fails
 *   the run. No backup is made when there are no subscribers. Successful edits
 *   stay in place; there is no second whole-state copy or commit assignment.
 * - loop is reserved: removing or changing any recovery/progress field is an
 *   error. In Ready, keep at least one turn if history was nonempty and retain
 *   valid message kinds and paired tool calls/results. Remove a complete step,
 *   or remove calls and corresponding results together, never just one side.
 * - In Projection/Blocked, history and recovery records must remain unchanged;
 *   other fields can be edited. Hooks cannot certify uncertain tool effects or
 *   manufacture recovery evidence. Prompt structure must still render.
 * - Validation checks structure, not truth or provider-specific semantics.
 *   The host owns retention policy and any archival requirement before pruning.
 *   Rollback affects state only; hook external effects cannot be rolled back.
 *
 * A hook stop request does not interrupt this synchronous edit transaction.
 * Valid edits commit before the loop next observes stop. Exchange counters count
 * performed work and are not recomputed from the possibly pruned history.
 */
struct EditOnStepFinished {
    model_io::AgentInputState& state;
};

/**
 * Edits the live state after terminal bookkeeping and before RunFinished.
 *
 * Runs once for every admitted invocation, including Failed, Cancelled and
 * ExchangeLimit. Rejected inputs and entry-time recovery failures do not publish it.
 * Uses the EditOnStepFinished edit/rollback contract. Applications include final
 * history compaction and updating host summaries or persistence metadata.
 * result is read-only and describes the outcome before this hook; editing state
 * cannot turn a Failed run into Completed or clear its recovery obligations.
 *
 * On failure, the edit is rolled back, the outcome becomes Failed with an
 * appended diagnostic, and RunFinished still observes that final outcome. This
 * event is not retried. RunFinished remains read-only and sees successful edits.
 */
struct EditOnRunFinished {
    model_io::AgentInputState& state;
    const RunResult& result;
};

/**
 * Observes the final, immutable outcome after EditOnRunFinished has completed.
 * A throwing subscriber stops this event's remaining subscribers and is logged;
 * it cannot change the already-final RunResult or state.loop. Persistence hooks
 * can therefore rely on the status they observe during their callback.
 */
struct RunFinished {
    const model_io::AgentInputState& state;
    const RunResult& result;
};

} // namespace loop
