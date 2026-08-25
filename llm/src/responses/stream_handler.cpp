// responses/stream_handler.cpp — the event decode of the Responses-API
// layer, and nothing else: one delta out per event in. The whole decode
// path is exception-total (guarded nlohmann access, non-throwing parse): a
// surprising server event surfaces as a delta, never as an exception —
// put()'s catch(...) stays reserved for genuine framing faults. The
// accumulation of these deltas into the contract record lives in
// responses/reader.cpp (ResponsesReader).

#include "llm/responses/stream_handler.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "llm/responses/event_access.hpp"

namespace llm::responses {

namespace {

// The shared guarded field access (event_access.hpp): never throws, so the
// decode path stays exception-total.
using detail::find_object;
using detail::get_index;
using detail::get_string;
using detail::summary_part_index;

// ---- delta constructors -------------------------------------------------------

ResponsesDelta text_delta(DeltaKind kind, const nlohmann::json& event) {
    ResponsesDelta delta;
    delta.kind = kind;
    delta.item_id = get_string(event, "item_id");
    delta.text = get_string(event, "delta");
    delta.output_index = get_index(event, "output_index");
    delta.content_index = get_index(event, "content_index");
    return delta;
}

ResponsesDelta marker(const nlohmann::json& event) {
    ResponsesDelta delta;
    delta.kind = DeltaKind::Marker;
    delta.text = get_string(event, "type");
    delta.extras = event;
    return delta;
}

ResponsesDelta ignored(std::string what, const nlohmann::json& event = {}) {
    ResponsesDelta delta;
    delta.kind = DeltaKind::Ignored;
    delta.text = std::move(what);
    if (event.is_object()) delta.extras = event;
    return delta;
}

// ---- explicit placeholders ----------------------------------------------------
//
// Event families / item kinds this layer deliberately does not handle
// (agent-loop/ReAct core only: text, reasoning, function calling, refusal).
// Every wire name is listed so a new server-side category shows up as a
// missing entry here — and as DeltaKind::Ignored on the stream — instead of
// vanishing into a silent default.

const std::array<const char*, 35> kIgnoredEventTypes = {
    // §7 file search
    "response.file_search_call.in_progress",
    "response.file_search_call.searching",
    "response.file_search_call.completed",
    // §8 web search
    "response.web_search_call.in_progress",
    "response.web_search_call.searching",
    "response.web_search_call.completed",
    // §11 image generation
    "response.image_generation_call.in_progress",
    "response.image_generation_call.generating",
    "response.image_generation_call.partial_image",
    "response.image_generation_call.completed",
    // §12 MCP
    "response.mcp_call_arguments.delta",
    "response.mcp_call_arguments.done",
    "response.mcp_call.in_progress",
    "response.mcp_call.completed",
    "response.mcp_call.failed",
    "response.mcp_list_tools.in_progress",
    "response.mcp_list_tools.completed",
    "response.mcp_list_tools.failed",
    // §13 code interpreter
    "response.code_interpreter_call.in_progress",
    "response.code_interpreter_call.interpreting",
    "response.code_interpreter_call.completed",
    "response.code_interpreter_call_code.delta",
    "response.code_interpreter_call_code.done",
    // §14 annotations
    "response.output_text.annotation.added",
    // §15 custom tools
    "response.custom_tool_call_input.delta",
    "response.custom_tool_call_input.done",
    // §16 audio
    "response.audio.delta",
    "response.audio.done",
    "response.audio.transcript.delta",
    "response.audio.transcript.done",
    // §17 shell
    "response.shell_call_command.added",
    "response.shell_call_command.delta",
    "response.shell_call_command.done",
    "response.shell_call_output_content.delta",
    "response.shell_call_output_content.done",
};

template<std::size_t N>
bool listed(const std::array<const char*, N>& names, const std::string& what) {
    return std::find(names.begin(), names.end(), what) != names.end();
}

bool is_ignored_event_type(const std::string& type) {
    return listed(kIgnoredEventTypes, type);
}

} // namespace

ResponsesDelta ResponsesStreamHandler::_handle_message(
    std::span<const LineInfo> message) {
    // Frame: per the SSE spec all `data:` lines join with '\n'; `event:`
    // names the type for frames that carry no data.
    std::string data;
    bool has_data = false;
    std::string event_name;
    for (const auto& [field, value] : message) {
        if (field == "data") {
            if (has_data) data += '\n';
            data += value;
            has_data = true;
        } else if (field == "event") {
            event_name = value;
        }
    }

    if (!has_data) {
        // event:-only frame: no payload to decode, but the boundary is still
        // visible to consumers as a marker.
        ResponsesDelta delta;
        delta.kind = DeltaKind::Marker;
        delta.text = event_name.empty() ? std::string("event") : event_name;
        return delta;
    }

    if (data == "[DONE]") {
        // Chat-completions sentinel some proxies append; a Responses stream
        // ends with response.completed instead. Explicit placeholder.
        return ignored("[DONE]");
    }

    nlohmann::json event =
        nlohmann::json::parse(data, nullptr, false);   // non-throwing
    if (event.is_discarded()) return ignored("unparsable-data");
    const nlohmann::json raw_event = event;
    try {
        event = _dialect->normalize_event(std::move(event));
    } catch (...) {
        return ignored("dialect-normalization-error", raw_event);
    }
    if (!event.is_object()) return ignored("non-object-event");
    const std::string type = get_string(event, "type");
    if (type.empty()) return ignored("missing-type");

    // ---- incremental channels (ReAct core) --------------------------------
    //
    // Each `.done` carries the COMPLETE text, so it decodes as a Marker
    // (full event in extras) — never as a second increment: the reader
    // overwrites its accumulator from the marker instead, and a live
    // consumer never sees the payload twice.

    if (type == "response.output_text.delta") {
        return text_delta(DeltaKind::Text, event);
    }
    if (type == "response.output_text.done") {
        return marker(event);
    }

    if (type == "response.refusal.delta") {
        return text_delta(DeltaKind::Refusal, event);
    }
    if (type == "response.refusal.done") {
        return marker(event);
    }

    if (type == "response.reasoning_text.delta") {
        return text_delta(DeltaKind::ReasoningText, event);
    }
    if (type == "response.reasoning_text.done") {
        return marker(event);
    }

    if (type == "response.reasoning_summary_text.delta") {
        ResponsesDelta delta = text_delta(DeltaKind::ReasoningSummary, event);
        // Resolve the part index into the delta (see summary_part_index) —
        // a server that spells it summary_index still identifies its part.
        if (!delta.content_index) {
            delta.content_index = summary_part_index(event);
        }
        return delta;
    }
    if (type == "response.reasoning_summary_text.done") {
        return marker(event);
    }

    if (type == "response.function_call_arguments.delta") {
        return text_delta(DeltaKind::ToolCallArgs, event);
    }
    if (type == "response.function_call_arguments.done") {
        return marker(event);
    }

    // ---- item lifecycle ------------------------------------------------------

    if (type == "response.output_item.added" || type == "response.output_item.done") {
        const nlohmann::json* item_json = find_object(event, "item");
        if (!item_json) return marker(event);   // lifecycle event, no payload

        // All item lifecycle records reach the reader. It strongly maps the
        // ReAct core and preserves every other completed item verbatim for
        // forward-compatible round-tripping.
        return marker(event);
    }

    if (type == "response.content_part.added" || type == "response.content_part.done") {
        const nlohmann::json* part = find_object(event, "part");
        const std::string part_type = part ? get_string(*part, "type") : std::string();
        if (part_type.empty() || part_type == "output_text" ||
            part_type == "refusal" || part_type == "reasoning_text") {
            return marker(event);
        }
        return ignored(type, event);
    }

    // ---- response lifecycle ----------------------------------------------------
    //
    // queued/created/in_progress and the terminal family all decode as
    // plain markers: the reader takes the response id, usage and terminal
    // details (incomplete_details / error) from the event JSON in extras,
    // sets its StreamStatus, and assembles.

    if (type == "response.queued" || type == "response.created" ||
        type == "response.in_progress" ||
        type == "response.completed" || type == "response.incomplete" ||
        type == "response.failed" || type == "response.cancelled" ||
        type == "error") {
        return marker(event);
    }

    if (type == "response.reasoning_summary_part.added" ||
        type == "response.reasoning_summary_part.done") {
        // The part lifecycle's done event carries the part's authoritative
        // text, indexed for the reader by summary_index/content_index.
        return marker(event);
    }

    // ---- explicit placeholders ---------------------------------------------------

    if (is_ignored_event_type(type)) return ignored(type, event);

    return ignored("unknown:" + type, event);
}

} // namespace llm::responses
