#pragma once

/**
 * @file core/event_admission.hpp
 * @brief Which admission class an emitted event belongs to.
 *
 * The outbox keeps ordinary feedback, query replies, latest-value metadata and
 * lifecycle events in separate budgets, so classification decides what a
 * polling client can and cannot starve. Two rules follow from the ledger's
 * purpose:
 *
 *   - lifecycle slots back the events a transcript must retain, including the
 *     terminal storage-failure diagnostic;
 *   - ordinary metadata keeps coalescing whatever the storage state, because a
 *     repeated status/options snapshot must never consume that reserve.
 */

#include "core/event_outbox.hpp"
#include "protocol/wire.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace core {

/**
 * Classify one event for admission.
 *
 * `storage_failed` promotes only `error`: a terminal storage diagnostic is a
 * lifecycle boundary, while every other error stays replayable metadata. The
 * four ordinary metadata names are unconditional.
 */
inline EventOutbox::Kind classify_event(protocol::EventName name,
                                        const nlohmann::json& data,
                                        bool storage_failed) {
    using protocol::EventName;
    switch (name) {
        case EventName::model_response:
        case EventName::tool_calls:
        case EventName::tool_results:
            return EventOutbox::Kind::Preview;
        case EventName::history:
        case EventName::answer:
        case EventName::history_error:
        case EventName::answer_error:
            return EventOutbox::Kind::Query;
        case EventName::persisted:
        case EventName::status:
        case EventName::options:
        case EventName::export_error:
            // Unconditional: these are replayable latest-value snapshots.
            return EventOutbox::Kind::Metadata;
        case EventName::error:
            // A terminal storage failure is a lifecycle boundary; every other
            // error is a replayable diagnostic.
            return storage_failed ? EventOutbox::Kind::Lifecycle
                                  : EventOutbox::Kind::Metadata;
        case EventName::compact_finished:
            // Automatic compaction is latest-value metadata; a manual result is
            // a lifecycle event the transcript must retain.
            return data.value("origin", std::string()) == "automatic"
                ? EventOutbox::Kind::Metadata : EventOutbox::Kind::Lifecycle;
        case EventName::ready:
        case EventName::input_admitted:
        case EventName::run_started:
        case EventName::input_committed:
        case EventName::run_finished:
            return EventOutbox::Kind::Lifecycle;
        case EventName::input_rejected:
            return EventOutbox::Kind::Feedback;
    }
    return EventOutbox::Kind::Feedback;
}

} // namespace core
