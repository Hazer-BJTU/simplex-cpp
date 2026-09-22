#pragma once

#include "loop/contracts.hpp"

namespace loop {

// Every reference below is borrowed for synchronous dispatch only. Subscribers
// must not retain it, mutate state through another alias, or reenter run().
// Writable events expose candidates; a throwing subscriber rejects the draft.

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
 * Observes the saved terminal state and its corresponding return summary.
 * A throwing subscriber changes the final state/result to Failed. The event is
 * not broadcast again, so earlier subscribers must consult the final state or
 * return value when they need the outcome of finish notification itself.
 */
struct RunFinished {
    const model_io::AgentInputState& state;
    const RunResult& result;
};

} // namespace loop
