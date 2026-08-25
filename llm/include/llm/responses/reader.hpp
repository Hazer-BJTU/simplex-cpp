#pragma once

//
// responses/reader.hpp — Responses-API stream consumer: accumulate + assemble
// ===========================================================================
//
// The consumer half of the Responses-API compatibility layer: a
// ModelResponseReader over ResponsesStreamHandler's delta stream (the
// handler decodes, this reader consumes). It drives get() through the
// base's next(), folds every delta into per-item accumulators, and — on the
// terminal marker — assembles the contract record, a model_io::MessageItem
// shaped as one ReAct model_response:
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
// Accumulation runs purely off the delta stream — the lossless view the
// decoder guarantees (stream_handler.hpp): increments append their bytes
// (the summary channel keyed by the delta's resolved part index), and each
// Marker re-plays its full event JSON, where the `*.done` overwrites, item
// lifecycle seeding and response-level id/usage/details live. One marker,
// one fold; never a second increment for a payload a `.done` already
// carries complete.
//
// StreamStatus::Aborted is this layer's addition to the wire's terminal
// states: the stream ended WITHOUT a terminal event — a transport/framing
// fault (the handler's ERROR path), an external finish(), abort(), or a
// reader-side exception (which next() rethrew after finishing the handler
// ERROR). On abort nothing is assembled: response() keeps whatever the
// last _assemble() left (usually none) — partial data dies with the stream.
//
// Concurrency: per the base class — this reader is the ONE consumer of its
// handler; response()/status() only after next() returned nullopt.
//
//

#include <cstddef>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "dataclass/model_io.hpp"
#include "endpoint/model_request.hpp"
#include "endpoint/request.hpp"
#include "llm/responses/delta.hpp"
#include "llm/responses/dialect.hpp"
#include "llm/responses/status.hpp"
#include "llm/responses/stream_handler.hpp"

namespace llm::responses {

/// Terminal state of one streamed response (see status()).
using StreamStatus = ResponseStatus;

/**
 * @brief Consumes a Responses-API delta stream into one MessageItem.
 *
 * See the header block for the delta->contract mapping and the abort
 * semantics, and ModelResponseReader for the consume loop, hooks and the
 * exception policy this inherits. One reader per response; it owns (or
 * adopts) the ResponsesStreamHandler whose stream it is the sole consumer
 * of — hand reader->handler() to sse_request on the producer side.
 */
class ResponsesReader
    : public endpoint::ModelResponseReader<ResponsesDelta> {
public:
    /// Create the decoder this reader will drain, and adopt it.
    explicit ResponsesReader(boost::asio::any_io_executor executor,
                             std::size_t line_window = endpoint::DEFAULT_SSE_LINE_WINDOW,
                             ResponsesDialectPtr dialect = default_dialect())
        : ModelResponseReader(std::make_shared<ResponsesStreamHandler>(
              std::move(executor), line_window, std::move(dialect))) {}

    explicit ResponsesReader(boost::asio::any_io_executor executor,
                             ResponsesDialectPtr dialect,
                             std::size_t line_window = endpoint::DEFAULT_SSE_LINE_WINDOW)
        : ResponsesReader(std::move(executor), line_window, std::move(dialect)) {}

    /// Adopt an already-created decoder (e.g. wired into a custom producer).
    explicit ResponsesReader(std::shared_ptr<ResponsesStreamHandler> handler)
        : ModelResponseReader(std::move(handler)) {}

    /// The assembled model_response. Meaningful only after the stream ended
    /// (see the concurrency note in the header block).
    const model_io::MessageItem& response() const noexcept override {
        return _response;
    }

    /// Which terminal state the stream reached — the wire's terminal event
    /// when one was seen, else Aborted (ended without one; see the header
    /// block). Meaningful only once finished().
    StreamStatus status() const noexcept {
        if (_status == StreamStatus::Streaming && finished()) {
            return StreamStatus::Aborted;
        }
        return _status;
    }

    ResponseStatus response_status() const noexcept { return status(); }

    const std::optional<nlohmann::json>& terminal_details() const noexcept {
        return _terminal_details;
    }

    /// The reuse reset: the base's stream/handler rewind plus this layer's
    /// accumulation state (per-item accumulators, the assembled response, the
    /// terminal status/usage/details). Hooks survive — see the base's clear().
    void clear() override {
        endpoint::ModelResponseReader<ResponsesDelta>::clear();
        _items.clear();
        _next_arrival = 0;
        _response = model_io::MessageItem{};
        _status = StreamStatus::Streaming;
        _response_id.clear();
        _usage.reset();
        _terminal_details.reset();
    }

protected:
    void _accumulate(const ResponsesDelta& delta) override;
    bool _is_terminal(const ResponsesDelta& delta) const override {
        return is_terminal(delta);   // delta.hpp — the delta-type predicate
    }
    void _assemble() override;

private:
    // Per-output-item accumulators, keyed by the item id (`item.id` on
    // output_item events, `item_id` on delta events — same key space).
    struct ItemState {
        // "not yet stamped" sentinel for arrival (0 is a legitimate stamp).
        static constexpr std::size_t kUnstamped = static_cast<std::size_t>(-1);

        std::string type;          // "message" | "reasoning" | "function_call"
        std::size_t output_index = 0;
        // Slot-creation sequence number: the tie-breaker when items share an
        // output_index (lifecycle-less orphans all default to 0), so _assemble
        // orders deterministically by (output_index, arrival) instead of the
        // id-lexicographic map order. Stamped on first touch by _item().
        std::size_t arrival = kUnstamped;
        // Content is independently indexed. A `.done` event is authoritative
        // only for its own part and must not overwrite sibling parts.
        std::map<std::size_t, std::string> text_parts;
        std::map<std::size_t, std::string> refusal_parts;
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

    ItemState& _item(
        const std::string& id,
        std::optional<std::size_t> output_index = std::nullopt);
    void _accumulate_output_item(const nlohmann::json& item,
                                 std::optional<std::size_t> output_index,
                                 bool authoritative);
    void _accumulate_marker(const nlohmann::json& event);
    void _set_terminal_status(const std::string& type);   // name -> StreamStatus

    std::map<std::string, ItemState> _items;
    std::size_t _next_arrival = 0;
    model_io::MessageItem _response;
    StreamStatus _status = StreamStatus::Streaming;
    std::string _response_id;
    std::optional<nlohmann::json> _usage;
    std::optional<nlohmann::json> _terminal_details; // incomplete_details | error
};

} // namespace llm::responses
