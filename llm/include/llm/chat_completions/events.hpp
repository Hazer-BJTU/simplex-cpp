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
 *   - **Retry replay**: one converse() exchange keeps one reasoning_id
 *     across transport retries; a retried attempt re-broadcasts its
 *     increments under the same id. Correlating (and deduplicating) by id
 *     is the subscriber's concern.
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
    /// Identifies the thinking pass: stable across the retries of one
    /// converse() exchange, fresh per exchange.
    std::string reasoning_id;
    /// The provider dialect's name ("deepseek"), empty for a generic
    /// no-dialect model.
    std::string provider;
    /// The model name the exchange was configured with.
    std::string model;
};

} // namespace llm::chat_completions
