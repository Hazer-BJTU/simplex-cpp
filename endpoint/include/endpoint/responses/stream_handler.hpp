#pragma once

//
// responses/stream_handler.hpp — Responses-API SSE stream decoder
// ===============================================================
//
// The dedicated SSEResponseHandler of the Responses-API compatibility layer:
// it decodes each Server-Sent Event of a `POST /responses` stream into ONE
// ResponsesDelta (the streaming view — see delta.hpp) while accumulating the
// same events into the contract record, a model_io::MessageItem shaped as
// one ReAct model_response:
//
//   output_text parts      -> content.raw          (+ refusal, see below)
//   reasoning_text channel -> reasoning.raw        (§10, raw thinking)
//   reasoning summaries    -> reasoning (fallback) (§9)
//   function_call items    -> invokes[] (id=call_id, arguments parsed)
//   completed items        -> extras: reasoning items + full output_items
//                             (round-trip: the interpreter re-emits these
//                             verbatim in later requests — the API docs
//                             mandate resending reasoning items, whose
//                             authoritative form is the output_item.done one)
//
// Refusal placement: when the response also produced output_text the refusal
// rides in content.extras["refusal"]; with no output_text it becomes
// content.raw. Ordering across kinds is inherently lossy (a MessageItem is
// one message): text concatenates, calls keep their output_index order.
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
// (response.failed / error) ends the stream through StreamStatus, not
// through an exception.
//
// Canonical consume loop (the handler never calls finish() itself — the
// consumer owns the lifecycle, cf. SSEResponseHandler class doc):
//
//     auto handler = std::make_shared<ResponsesStreamHandler>(executor);
//     // producer: sse_request(handler, std::move(stream),
//     //                        interpreter->build_request(...));
//     for (;;) {
//         ResponsesDelta delta = co_await handler->get();  // throws SSEAborted
//         if (ResponsesStreamHandler::is_terminal(delta)) break;
//     }
//     handler->finish();                    // DONE
//     consume(handler->response());         // the assembled MessageItem
//
// Concurrency: response()/status()/finished() read unsynchronized
// accumulators — call them only once the stream has ended and the producer
// coroutine has exited (or from the producer side), per the SPSC discipline
// of SSEResponseHandler.
//

#include <cstddef>
#include <iosfwd>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dataclass/model_io.hpp"
#include "endpoint/request.hpp"
#include "endpoint/responses/delta.hpp"

namespace endpoint::responses {

/// Terminal state of one streamed response (see finished()/status()).
enum class StreamStatus {
    Streaming,  ///< no terminal event yet.
    Completed,  ///< response.completed.
    Incomplete, ///< response.incomplete (truncation etc.).
    Failed,     ///< response.failed.
    Errored,    ///< a bare `error` event.
};

// Streaming for diagnostics (Boost.Test assertions, logging).
inline std::ostream& operator<<(std::ostream& os, StreamStatus status) {
    switch (status) {
        case StreamStatus::Streaming:  return os << "Streaming";
        case StreamStatus::Completed:  return os << "Completed";
        case StreamStatus::Incomplete: return os << "Incomplete";
        case StreamStatus::Failed:     return os << "Failed";
        case StreamStatus::Errored:    return os << "Errored";
    }
    return os << "StreamStatus(?)";
}

/**
 * @brief Decodes a Responses-API SSE stream into deltas + one MessageItem.
 *
 * See the header block for the event mapping, the explicit-placeholder
 * policy and the consume loop. Stateless per instance beyond the stream it
 * is decoding: one handler per response. Not final: PeekingHandler composes
 * by inheritance (the only intended subclass shape).
 */
class ResponsesStreamHandler
    : public endpoint::SSEResponseHandler<ResponsesDelta> {
public:
    using endpoint::SSEResponseHandler<ResponsesDelta>::SSEResponseHandler;

    /// True when this marker delta is a terminal lifecycle event
    /// (completed / incomplete / failed / error) — the consumer's break.
    static bool is_terminal(const ResponsesDelta& delta);

    /// The assembled model_response. Meaningful only after the stream ended
    /// (see the concurrency note in the header block).
    const model_io::MessageItem& response() const noexcept { return _response; }

    /// Whether a terminal lifecycle event has been seen.
    bool finished() const noexcept { return _status != StreamStatus::Streaming; }

    /// Which terminal state the stream reached.
    StreamStatus status() const noexcept { return _status; }

protected:
    ResponsesDelta _handle_message(std::span<const LineInfo> message) override;

private:
    // Per-output-item accumulators, keyed by the item id (`item.id` on
    // output_item events, `item_id` on delta events — same key space).
    struct ItemState {
        std::string type;          // "message" | "reasoning" | "function_call"
        std::size_t output_index = 0;
        std::string text;          // message output_text / reasoning_text channel
        std::string refusal;       // message refusal part
        // Reasoning summaries arrive as INDEXED parts (summary_index on part
        // lifecycle events, content_index on the text channel): kept separate
        // so one part's text (and its .done overwrite) cannot clobber
        // another's. _assemble joins them in index order.
        std::map<std::size_t, std::string> summary_parts;
        std::string call_id, name, arguments;  // function_call
        // Full item json from output_item.done (the authoritative version);
        // null until captured — is_object() doubles as the captured flag.
        nlohmann::json done_item;
    };

    ItemState& _item(const std::string& id);  // find-or-create, lenient
    void _assemble();                         // terminal event -> MessageItem

    std::map<std::string, ItemState> _items;
    model_io::MessageItem _response;
    StreamStatus _status = StreamStatus::Streaming;
    std::string _response_id;
    std::optional<nlohmann::json> _usage;
    std::optional<nlohmann::json> _terminal_details; // incomplete_details | error
};

} // namespace endpoint::responses
