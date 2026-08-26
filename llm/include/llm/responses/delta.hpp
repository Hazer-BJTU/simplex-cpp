#pragma once

//
// responses/delta.hpp — the streaming product of the Responses-API layer
// =====================================================================
//
// One ResponsesDelta per Server-Sent Event off a `POST /responses` stream:
// ResponsesStreamHandler decodes each event into exactly one delta (an
// invariant — even explicitly ignored categories surface as DeltaKind::Ignored,
// never as silence) and publishes it through SSEResponseHandler's channel.
// The handler simultaneously accumulates the same events into the contract
// record (model_io::MessageItem); deltas are the *transient* streaming view,
// deliberately NOT part of the JSON data contract and never serialised —
// like generation parameters, they are stream-local data.
//
// Kinds split the wire's incremental channels; note the reasoning split: the
// API streams reasoning *text* (§10, the raw channel) and reasoning
// *summaries* (§9) as separate event families with separate accumulators, so
// a monitor can choose which to show. Hot text increments carry only their
// bytes — full event JSON rides in `extras` on Marker/Ignored only, keeping
// the per-token cost a string append.
//

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace llm::responses {

/// Which incremental channel a ResponsesDelta belongs to.
enum class DeltaKind {
    Text,             // response.output_text.delta — visible assistant text.
    ReasoningText,    // response.reasoning_text.delta — raw thinking channel.
    ReasoningSummary, // response.reasoning_summary_text.delta — summary channel.
    ToolCallArgs,     // response.function_call_arguments.delta — JSON string fragments.
    Refusal,          // response.refusal.delta — refusal text.
    Marker,           // Lifecycle milestone (item/part boundaries, `*.done`, terminals); carries the full event in extras.
    Ignored,          // Explicit placeholder for categories this layer does not handle; text names the event type.
};

/// One decoded SSE event of a Responses-API stream (see header).
struct ResponsesDelta {
    DeltaKind kind = DeltaKind::Marker;
    /// The output item this delta belongs to (empty for response-level events).
    std::string item_id;
    /// The increment bytes — or, for Marker/Ignored, the event type name.
    std::string text;
    /// Wire indices, when the event carried them. For ReasoningSummary
    /// deltas this is the RESOLVED summary part index (content_index, else
    /// summary_index, else 0) — the decoder resolves it so the delta alone
    /// identifies the part it belongs to.
    std::optional<std::size_t> output_index, content_index;
    /// Full event JSON. Populated on Marker/Ignored only (hot increments
    /// stay lean); markers for output_item events expose e.g. the tool name.
    std::optional<nlohmann::json> extras;
};

/// True when this marker delta is a terminal lifecycle event
/// (completed / incomplete / failed / cancelled / error) — the consumer's break, i.e.
/// ModelResponseReader::_is_terminal for this delta type.
inline bool is_terminal(const ResponsesDelta& delta) {
    if (delta.kind != DeltaKind::Marker) return false;
    return delta.text == "response.completed" ||
           delta.text == "response.incomplete" ||
           delta.text == "response.failed" ||
           delta.text == "response.cancelled" ||
           // Bare spellings, accepted by the reader's _set_terminal_status
           // for data-less `event:`-only frames — keep the two tables in
           // lockstep or the stream terminates without assembling.
           delta.text == "cancelled" ||
           delta.text == "error";
}

// Streaming for diagnostics (Boost.Test assertions, logging).
inline std::ostream& operator<<(std::ostream& os, DeltaKind kind) {
    switch (kind) {
        case DeltaKind::Text:             return os << "Text";
        case DeltaKind::ReasoningText:    return os << "ReasoningText";
        case DeltaKind::ReasoningSummary: return os << "ReasoningSummary";
        case DeltaKind::ToolCallArgs:     return os << "ToolCallArgs";
        case DeltaKind::Refusal:          return os << "Refusal";
        case DeltaKind::Marker:           return os << "Marker";
        case DeltaKind::Ignored:          return os << "Ignored";
    }
    return os << "DeltaKind(?)";
}

} // namespace llm::responses
