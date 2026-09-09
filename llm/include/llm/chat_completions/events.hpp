/**
 * @file events.hpp
 * @brief The chat-completions adapter's broadcast events — live observation
 *        of a running model exchange through the process-wide event bus.
 *
 * `ReasoningDeltaEvent` is published per streamed reasoning increment, from
 * inside `ChatCompletionsModel::converse()`, on the reader hook the model
 * registers by default. It is the sanctioned live view the LLMModel contract
 * plans for (llm/models.hpp: streaming observation routes through a
 * process-wide event sink): any code — the CLI's stderr view, a UI, a
 * tracer — subscribes on `eventbus::default_bus()` and watches every
 * provider's output without touching the exchange.
 *
 * Contract:
 *
 *   - **Synchronous**: slots run inline on the publishing thread (the I/O
 *     executor inside converse), in wire order, as the deltas arrive.
 *   - **No subscribers = no-op**: the bus drops the event silently; the
 *     exchange is unaffected (llm/utils/eventbus guarantees).
 *   - **Slots must not throw**: a throwing slot propagates into the
 *     exchange and fails it — observers are witnesses, not participants.
 *   - **Concurrency**: converse() is reentrant, so events from SEVERAL
 *     exchanges interleave arbitrarily — and on a multi-threaded executor
 *     they are published from different threads INTO THE SAME SLOT (the bus
 *     releases its registry lock before running slots). A subscriber must
 *     therefore (a) be thread-safe itself, and (b) demultiplex by
 *     `exchange_id` rather than assuming one live stream. `provider` and
 *     `model` do NOT separate concurrent calls on one model — they are
 *     identical across them; `exchange_id` is the only join key.
 *   - **Retry replay**: one converse() exchange keeps one exchange_id
 *     across transport retries; a retried attempt re-broadcasts its
 *     increments under the same id, with `attempt` incremented. A
 *     subscriber accumulating text should DISCARD what it holds for an
 *     exchange_id whenever `attempt` advances — the wire is replaying from
 *     the beginning, so appending would duplicate the prefix.
 *   - **Binding a stream to its result**: the assembled MessageItem
 *     converse() returns carries the same id under `extras.exchange_id`, so
 *     a subscriber's buffer can be matched to the exchange's outcome.
 *   - **Across the plugin boundary**: the event type compiles from this
 *     header on both sides, so type routing is name-based and boundary-
 *     safe. The *bus instance* is unique by construction: default_bus()
 *     lives in the SHARED eventbus library, and a host executable and its
 *     dlopened provider plugins bind the same SONAME — one bus per
 *     process. (A module that forgets to link it fails at link time,
 *     loudly, rather than publishing into a private second bus.)
 */

#pragma once

#include <string>

namespace llm::chat_completions {

/// One streamed reasoning (`reasoning_content`) increment of one exchange.
struct ReasoningDeltaEvent {
    /// The increment text, verbatim from the wire delta.
    std::string reasoning;
    /// The exchange this increment belongs to — THE join key under
    /// concurrency (llm/exchange_id.hpp). Stable across the retries of one
    /// converse() exchange, unique per exchange process-wide, and repeated
    /// on the returned MessageItem's `extras.exchange_id`.
    std::string exchange_id;
    /// Which transport attempt produced this increment: 0 for the initial
    /// exchange, 1.. for retries. Advances only when endpoint::complete
    /// re-reads the stream from scratch — the signal to drop the partial
    /// text accumulated for this exchange_id and start over.
    unsigned attempt = 0;
    /// The provider dialect's name ("deepseek"), empty for a generic
    /// no-dialect model. Informational: it does NOT distinguish concurrent
    /// exchanges on one model.
    std::string provider;
    /// The model name the exchange was configured with. Informational, as
    /// above — the snapshot this exchange runs against, so a concurrent
    /// set_generation() cannot change it mid-stream.
    std::string model;
};

} // namespace llm::chat_completions
