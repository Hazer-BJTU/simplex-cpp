// responses/stream_handler.cpp — the event decode + accumulation of the
// Responses-API layer. The whole decode path is exception-total (guarded
// nlohmann access, non-throwing parse): a surprising server event surfaces
// as a delta, never as an exception — put()'s catch(...) stays reserved for
// genuine framing faults.

#include "endpoint/responses/stream_handler.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace endpoint::responses {

namespace {

// ---- guarded field access (never throws) ------------------------------------

const nlohmann::json* find_object(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return nullptr;
    return &*it;
}

std::string get_string(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return {};
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

std::optional<std::size_t> get_index(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return std::nullopt;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) return std::nullopt;
    return it->get<std::size_t>();
}

// The reasoning-summary part an event belongs to: content_index on the
// summary-text channel (the documented field), summary_index on the part
// lifecycle events, 0 when a server omits both (single-part degenerate case).
std::size_t summary_part_index(const nlohmann::json& event) {
    if (auto index = get_index(event, "content_index")) return *index;
    if (auto index = get_index(event, "summary_index")) return *index;
    return 0;
}

// Join a reasoning item's summary parts in index order, blank paragraphs
// apart — the readable flattening of the wire's summary[] array into the one
// reasoning.raw fallback string.
std::string join_summary_parts(
    const std::map<std::size_t, std::string>& parts) {
    std::string joined;
    for (const auto& [index, text] : parts) {
        if (text.empty()) continue;
        if (!joined.empty()) joined += "\n\n";
        joined += text;
    }
    return joined;
}

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

const std::array<const char*, 13> kIgnoredItemTypes = {
    "file_search_call",
    "web_search_call",
    "mcp_call",
    "mcp_list_tools",
    "code_interpreter_call",
    "image_generation_call",
    "shell_call",
    "custom_tool_call",
    "custom_tool_call_output",
    "computer_call",
    "tool_search_call",
    "compaction",
    "item_reference",
};

template<std::size_t N>
bool listed(const std::array<const char*, N>& names, const std::string& what) {
    return std::find(names.begin(), names.end(), what) != names.end();
}

bool is_ignored_event_type(const std::string& type) {
    return listed(kIgnoredEventTypes, type);
}

} // namespace

bool ResponsesStreamHandler::is_terminal(const ResponsesDelta& delta) {
    if (delta.kind != DeltaKind::Marker) return false;
    return delta.text == "response.completed" ||
           delta.text == "response.incomplete" ||
           delta.text == "response.failed" ||
           delta.text == "error";
}

ResponsesStreamHandler::ItemState& ResponsesStreamHandler::_item(
    const std::string& id) {
    // Deltas may legitimately arrive before output_item.added (or without it
    // ever coming): open an untyped slot rather than losing the bytes.
    return _items[id];
}

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

    const nlohmann::json event =
        nlohmann::json::parse(data, nullptr, false);   // non-throwing
    if (event.is_discarded()) return ignored("unparsable-data");
    const std::string type = get_string(event, "type");
    if (type.empty()) return ignored("missing-type");

    // ---- incremental channels (ReAct core) --------------------------------

    if (type == "response.output_text.delta") {
        ItemState& item = _item(get_string(event, "item_id"));
        item.text += get_string(event, "delta");
        return text_delta(DeltaKind::Text, event);
    }
    if (type == "response.output_text.done") {
        // `.done` carries the COMPLETE text: overwrite (the increments were
        // already accumulated), and consumers see a Marker — emitting the
        // full text as another Text delta would print everything twice.
        ItemState& item = _item(get_string(event, "item_id"));
        std::string full = get_string(event, "text");
        if (!full.empty()) item.text = std::move(full);
        return marker(event);
    }

    if (type == "response.refusal.delta") {
        ItemState& item = _item(get_string(event, "item_id"));
        item.refusal += get_string(event, "delta");
        return text_delta(DeltaKind::Refusal, event);
    }
    if (type == "response.refusal.done") {
        ItemState& item = _item(get_string(event, "item_id"));
        std::string full = get_string(event, "refusal");
        if (!full.empty()) item.refusal = std::move(full);
        return marker(event);
    }

    if (type == "response.reasoning_text.delta") {
        ItemState& item = _item(get_string(event, "item_id"));
        item.text += get_string(event, "delta");
        return text_delta(DeltaKind::ReasoningText, event);
    }
    if (type == "response.reasoning_text.done") {
        ItemState& item = _item(get_string(event, "item_id"));
        std::string full = get_string(event, "text");
        if (!full.empty()) item.text = std::move(full);
        return marker(event);
    }

    if (type == "response.reasoning_summary_text.delta") {
        ItemState& item = _item(get_string(event, "item_id"));
        item.summary_parts[summary_part_index(event)] +=
            get_string(event, "delta");
        return text_delta(DeltaKind::ReasoningSummary, event);
    }
    if (type == "response.reasoning_summary_text.done") {
        // The done text is authoritative for ITS part only (the increments
        // were already accumulated); other parts keep their own text.
        ItemState& item = _item(get_string(event, "item_id"));
        std::string full = get_string(event, "text");
        if (!full.empty()) {
            item.summary_parts[summary_part_index(event)] = std::move(full);
        }
        return marker(event);
    }

    if (type == "response.function_call_arguments.delta") {
        ItemState& item = _item(get_string(event, "item_id"));
        item.arguments += get_string(event, "delta");
        return text_delta(DeltaKind::ToolCallArgs, event);
    }
    if (type == "response.function_call_arguments.done") {
        ItemState& item = _item(get_string(event, "item_id"));
        std::string arguments = get_string(event, "arguments");
        if (!arguments.empty()) item.arguments = std::move(arguments);
        std::string name = get_string(event, "name");
        if (!name.empty()) item.name = std::move(name);
        return marker(event);
    }

    // ---- item lifecycle ------------------------------------------------------

    if (type == "response.output_item.added" || type == "response.output_item.done") {
        const nlohmann::json* item_json = find_object(event, "item");
        if (!item_json) return marker(event);   // lifecycle event, no payload
        const std::string item_type = get_string(*item_json, "type");
        const bool done = type == "response.output_item.done";

        if (item_type == "message" || item_type == "reasoning" ||
            item_type == "function_call") {
            ItemState& item = _item(get_string(*item_json, "id"));
            item.type = item_type;
            if (auto index = get_index(event, "output_index")) {
                item.output_index = *index;
            }
            if (item_type == "function_call") {
                // Seed/refresh the correlation + name from the item json.
                std::string call_id = get_string(*item_json, "call_id");
                if (!call_id.empty()) item.call_id = std::move(call_id);
                std::string name = get_string(*item_json, "name");
                if (!name.empty()) item.name = std::move(name);
                std::string arguments = get_string(*item_json, "arguments");
                if (!arguments.empty()) item.arguments = std::move(arguments);
            }
            if (done) {
                // The completed item is the authoritative version (the
                // `added` reasoning item may lack encrypted_content) —
                // capture it whole for round-tripping in later requests.
                item.done_item = *item_json;
            }
            return marker(event);
        }

        if (listed(kIgnoredItemTypes, item_type)) return ignored(type, event);
        return ignored("unknown-item:" + item_type, event);
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

    if (type == "response.queued" || type == "response.created" ||
        type == "response.in_progress") {
        if (const nlohmann::json* response = find_object(event, "response")) {
            std::string id = get_string(*response, "id");
            if (!id.empty()) _response_id = std::move(id);
        }
        return marker(event);
    }

    if (type == "response.completed" || type == "response.incomplete" ||
        type == "response.failed" || type == "error") {
        if (const nlohmann::json* response = find_object(event, "response")) {
            std::string id = get_string(*response, "id");
            if (!id.empty()) _response_id = std::move(id);
            if (const nlohmann::json* usage = find_object(*response, "usage")) {
                _usage = *usage;
            }
            if (const nlohmann::json* details =
                    find_object(*response, "incomplete_details")) {
                _terminal_details = *details;
            }
            if (const nlohmann::json* error = find_object(*response, "error")) {
                _terminal_details = *error;
            }
        }
        if (type == "error") {
            // A bare error event IS the error record (code/message/param).
            _terminal_details = event;
        }
        _status = type == "response.completed" ? StreamStatus::Completed
                : type == "response.incomplete" ? StreamStatus::Incomplete
                : type == "response.failed"     ? StreamStatus::Failed
                                                : StreamStatus::Errored;
        _assemble();
        return marker(event);
    }

    if (type == "response.reasoning_summary_part.added" ||
        type == "response.reasoning_summary_part.done") {
        // The part lifecycle's done event carries the part's authoritative
        // text; capture it into its indexed slot (added carries none).
        if (type == "response.reasoning_summary_part.done") {
            ItemState& item = _item(get_string(event, "item_id"));
            if (const nlohmann::json* part = find_object(event, "part")) {
                std::string text = get_string(*part, "text");
                if (!text.empty()) {
                    item.summary_parts[summary_part_index(event)] =
                        std::move(text);
                }
            }
        }
        return marker(event);
    }

    // ---- explicit placeholders ---------------------------------------------------

    if (is_ignored_event_type(type)) return ignored(type, event);

    return ignored("unknown:" + type, event);
}

void ResponsesStreamHandler::_assemble() {
    // Order the items by their wire output_index so parallel calls keep call
    // order and texts concatenate in production order.
    std::vector<const ItemState*> ordered;
    ordered.reserve(_items.size());
    for (const auto& [id, item] : _items) ordered.push_back(&item);
    std::sort(ordered.begin(), ordered.end(),
              [](const ItemState* a, const ItemState* b) {
                  return a->output_index < b->output_index;
              });

    model_io::MessageItem result;
    result.type = model_io::MessageItemType::ModelResponse;
    result.role = "assistant";

    std::string text, refusal, reasoning_text, summary;
    std::vector<nlohmann::json> reasoning_items, output_items;
    std::vector<model_io::InvokeQuery> invokes;

    // Per-kind folding of one item into the single assembled message.
    const auto as_message = [&](const ItemState& item) {
        text += item.text;
        refusal += item.refusal;
    };
    const auto as_reasoning = [&](const ItemState& item) {
        reasoning_text += item.text;
        summary += join_summary_parts(item.summary_parts);
        if (item.done_item.is_object()) reasoning_items.push_back(item.done_item);
    };
    const auto as_call = [&](const ItemState& item) {
        model_io::InvokeQuery query;
        query.id = item.call_id;
        query.name = item.name;
        // The wire carries arguments as a JSON STRING; parse now that
        // they are complete, keeping the raw string when malformed.
        if (!item.arguments.empty()) {
            nlohmann::json parsed =
                nlohmann::json::parse(item.arguments, nullptr, false);
            query.arguments = parsed.is_discarded()
                ? nlohmann::json(item.arguments)
                : std::move(parsed);
        }
        if (item.done_item.is_object()) query.extras = item.done_item;
        invokes.push_back(std::move(query));
    };

    for (const ItemState* item : ordered) {
        if (item->type == "message") {
            as_message(*item);
        } else if (item->type == "reasoning") {
            as_reasoning(*item);
        } else if (item->type == "function_call") {
            as_call(*item);
        } else if (item->type.empty()) {
            // Untyped slot — deltas arrived but output_item.added/done never
            // did (the leniency _item() documents). Salvage by best-effort
            // routing instead of dropping the bytes: call fields make it a
            // function call, summary parts make it reasoning, and anything
            // else with accumulated bytes is message text. A slot with
            // nothing accumulated has nothing to salvage.
            if (!item->call_id.empty() || !item->name.empty() ||
                !item->arguments.empty()) {
                as_call(*item);
            } else if (!join_summary_parts(item->summary_parts).empty()) {
                as_reasoning(*item);
            } else if (!item->text.empty() || !item->refusal.empty()) {
                as_message(*item);
            }
        }
        // Every completed item — handled kind or not — is recorded for
        // round-tripping; the interpreter re-emits the kinds it maps.
        if (item->done_item.is_object()) output_items.push_back(item->done_item);
    }

    result.content.type = model_io::ContentType::Text;
    result.content.raw = std::move(text);
    if (!refusal.empty()) {
        // Refusal is a distinct part kind: keep it out of the visible text
        // when there is any; only a refusal-only response surfaces it there.
        if (result.content.raw.empty()) {
            result.content.raw = std::move(refusal);
        } else {
            result.content.extras = nlohmann::json{{"refusal", std::move(refusal)}};
        }
    }

    if (!reasoning_text.empty() || !summary.empty() || !reasoning_items.empty()) {
        model_io::Content reasoning;
        reasoning.type = model_io::ContentType::Text;
        reasoning.raw = !reasoning_text.empty() ? reasoning_text : summary;
        if (!reasoning_items.empty()) {
            reasoning.extras = nlohmann::json{{"items", std::move(reasoning_items)}};
        }
        result.reasoning = std::move(reasoning);
    }

    if (!invokes.empty()) result.invokes = std::move(invokes);

    nlohmann::json extras = nlohmann::json::object();
    if (!_response_id.empty()) extras["response_id"] = _response_id;
    if (_usage) extras["usage"] = *_usage;
    if (_terminal_details) {
        // Truncation details vs. failure record — named by what happened.
        extras[_status == StreamStatus::Incomplete ? "incomplete_details" : "error"] =
            *_terminal_details;
    }
    if (!output_items.empty()) extras["output_items"] = std::move(output_items);
    if (!extras.empty()) result.extras = std::move(extras);

    _response = std::move(result);
}

} // namespace endpoint::responses
