#pragma once

//
// responses/stream_handler.hpp — Responses-API SSE stream decoder
// ===============================================================
//
// The dedicated SSEResponseHandler of the Responses-API compatibility layer:
// it decodes each Server-Sent Event of a `POST /responses` stream into ONE
// ResponsesDelta (the streaming view — see delta.hpp) and does NOTHING
// else. Accumulating the deltas into the contract record, assembling the
// final MessageItem, driving get(), finishing the handler, and observing
// the stream through hooks all live one layer up, in ResponsesReader
// (reader.hpp) over ModelResponseReader — this handler is a pure function
// of its events, so the decode stays provably free of consumer-side state.
//
// One ResponsesDelta per event, always — the invariant the reader's
// accumulation depends on: even explicitly ignored categories surface as
// DeltaKind::Ignored, never as silence. The delta stream is a complete,
// lossless view of the event stream: hot increments carry their bytes
// (resolved indices included, e.g. the reasoning-summary part index), and
// every Marker/Ignored carries the full event JSON in `extras` — enough
// for the reader to reconstruct everything this layer round-trips:
//
//   output_text parts      -> Text deltas            (content.raw)
//   reasoning_text channel -> ReasoningText deltas   (reasoning.raw, §10)
//   reasoning summaries    -> ReasoningSummary deltas (§9, per part index)
//   function_call args     -> ToolCallArgs deltas    (JSON string fragments)
//   refusal channel        -> Refusal deltas
//   item/response lifecycle-> Markers (full event in extras)
//
// Scope — agent loop / ReAct core only. Every event family this layer does
// not handle (file search, web search, MCP, code interpreter, image
// generation, custom tools, audio, shell, annotations, …) is an EXPLICIT
// placeholder: each is listed by name in the dispatch and surfaces as
// DeltaKind::Ignored, as do unknown event types ("unknown:<type>") and
// undecodable frames. Nothing is dropped silently.
//
// Error model: a malformed or surprising SERVER EVENT is not a stream fault
// — the decode path is exception-total (guarded nlohmann access only) and
// never throws, so put()'s catch(...) (which would finish(ERROR) both sides)
// stays reserved for genuine framing faults. A server-side terminal error
// (response.failed / error) is just another Marker delta; whether it ends
// the stream is the reader's call (is_terminal, delta.hpp).
//
// Canonical wiring (the consumer side is the reader's, cf. reader.hpp):
//
//     auto reader = std::make_shared<ResponsesReader>(executor);
//     // producer: sse_request(reader->handler(), std::move(stream),
//     //                        interpreter->build_request(...));
//     while (auto delta = co_await reader->next()) { ... }
//     consume(reader->response());
//
//

#include <span>
#include <string>

#include <nlohmann/json.hpp>

#include "endpoint/request.hpp"
#include "endpoint/responses/delta.hpp"

namespace endpoint::responses {

/**
 * @brief Decodes a Responses-API SSE stream into one delta per event.
 *
 * See the header block for the event mapping, the explicit-placeholder
 * policy and the lossless-delta invariant. Stateless beyond the framing
 * state it inherits: one handler per response, nothing accumulated — and
 * final by design: the consumer half of this layer is ResponsesReader, not
 * a subclass.
 */
class ResponsesStreamHandler final
    : public endpoint::SSEResponseHandler<ResponsesDelta> {
public:
    using endpoint::SSEResponseHandler<ResponsesDelta>::SSEResponseHandler;

protected:
    ResponsesDelta _handle_message(std::span<const LineInfo> message) override;
};

} // namespace endpoint::responses
