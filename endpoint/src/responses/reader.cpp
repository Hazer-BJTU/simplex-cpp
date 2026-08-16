// responses/reader.cpp — the delta accumulation + assembly of the
// Responses-API layer. Fed exclusively by the delta stream the decoder
// guarantees lossless (one delta per event, full event JSON on markers), so
// every fold here is exception-free bookkeeping: guarded nlohmann access
// only, never throws — a throw is a reader bug that next() escalates to a
// stream fault (finish(ERROR)) after the fact.

#include "endpoint/responses/reader.hpp"

#include <algorithm>
#include <utility>

#include "endpoint/responses/event_access.hpp"

namespace endpoint::responses {

namespace {

// The shared guarded field access (event_access.hpp) — the SAME helpers,
// the SAME summary-part resolution order, as the decoder uses; divergent
// private copies here would send a part's increments and its `.done`
// overwrite to different accumulators.
using detail::find_object;
using detail::get_index;
using detail::get_string;
using detail::summary_part_index;

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

} // namespace

ResponsesReader::ItemState& ResponsesReader::_item(const std::string& id) {
    // Deltas may legitimately arrive before output_item.added (or without it
    // ever coming): open an untyped slot rather than losing the bytes.
    ItemState& slot = _items[id];
    if (slot.arrival == ItemState::kUnstamped) {
        // First touch: record the creation order _assemble uses to break
        // output_index ties.
        slot.arrival = _next_arrival++;
    }
    return slot;
}

void ResponsesReader::_accumulate(const ResponsesDelta& delta) {
    switch (delta.kind) {
        // ---- increments: append their bytes ------------------------------
        // The delta's output_index (when the event carried one) keeps even
        // lifecycle-less slots ordered: it is the only ordering signal an
        // orphan item ever gets.
        case DeltaKind::Text:
        case DeltaKind::ReasoningText: {
            ItemState& item = _item(delta.item_id);
            if (delta.output_index) item.output_index = *delta.output_index;
            item.text += delta.text;
            return;
        }
        case DeltaKind::Refusal: {
            ItemState& item = _item(delta.item_id);
            if (delta.output_index) item.output_index = *delta.output_index;
            item.refusal += delta.text;
            return;
        }
        case DeltaKind::ToolCallArgs: {
            ItemState& item = _item(delta.item_id);
            if (delta.output_index) item.output_index = *delta.output_index;
            item.arguments += delta.text;
            return;
        }
        case DeltaKind::ReasoningSummary: {
            ItemState& item = _item(delta.item_id);
            if (delta.output_index) item.output_index = *delta.output_index;
            item.summary_parts[delta.content_index.value_or(0)] += delta.text;
            return;
        }

        // ---- nothing this layer accumulates ------------------------------
        case DeltaKind::Ignored:
            return;

        // ---- markers: fold the event JSON they carry ---------------------
        case DeltaKind::Marker:
            break;
    }
    if (delta.extras && delta.extras->is_object()) {
        _accumulate_marker(*delta.extras);
        return;
    }
    // A data-less marker (an `event:`-only frame): no payload to fold, but
    // a terminal NAME still fixes the status — is_terminal() fires on the
    // bare `event: error` spelling, and the caller must not see such a
    // server-side failure misreported as a transport Aborted.
    _set_terminal_status(delta.text);
}

void ResponsesReader::_set_terminal_status(const std::string& type) {
    if (type == "response.completed") _status = StreamStatus::Completed;
    else if (type == "response.incomplete") _status = StreamStatus::Incomplete;
    else if (type == "response.failed") _status = StreamStatus::Failed;
    else if (type == "error") _status = StreamStatus::Errored;
}

void ResponsesReader::_accumulate_marker(const nlohmann::json& event) {
    const std::string type = get_string(event, "type");

    // ---- `*.done` overwrites -------------------------------------------
    //
    // Each done payload is authoritative and COMPLETE; the increments were
    // already accumulated, so overwrite — never append. (An empty payload
    // overwrites nothing: absent fields are not declarations of emptiness.)

    if (type == "response.output_text.done") {
        ItemState& item = _item(get_string(event, "item_id"));
        std::string full = get_string(event, "text");
        if (!full.empty()) item.text = std::move(full);
        return;
    }
    if (type == "response.refusal.done") {
        ItemState& item = _item(get_string(event, "item_id"));
        std::string full = get_string(event, "refusal");
        if (!full.empty()) item.refusal = std::move(full);
        return;
    }
    if (type == "response.reasoning_text.done") {
        ItemState& item = _item(get_string(event, "item_id"));
        std::string full = get_string(event, "text");
        if (!full.empty()) item.text = std::move(full);
        return;
    }
    if (type == "response.reasoning_summary_text.done") {
        // Authoritative for ITS part only; other parts keep their own text.
        ItemState& item = _item(get_string(event, "item_id"));
        std::string full = get_string(event, "text");
        if (!full.empty()) {
            item.summary_parts[summary_part_index(event)] = std::move(full);
        }
        return;
    }
    if (type == "response.function_call_arguments.done") {
        ItemState& item = _item(get_string(event, "item_id"));
        std::string arguments = get_string(event, "arguments");
        if (!arguments.empty()) item.arguments = std::move(arguments);
        std::string name = get_string(event, "name");
        if (!name.empty()) item.name = std::move(name);
        return;
    }
    if (type == "response.reasoning_summary_part.done") {
        // The part lifecycle's done event carries the part's authoritative
        // text; capture it into its indexed slot.
        ItemState& item = _item(get_string(event, "item_id"));
        if (const nlohmann::json* part = find_object(event, "part")) {
            std::string text = get_string(*part, "text");
            if (!text.empty()) {
                item.summary_parts[summary_part_index(event)] =
                    std::move(text);
            }
        }
        return;
    }

    // ---- item lifecycle ------------------------------------------------------

    if (type == "response.output_item.added" || type == "response.output_item.done") {
        const nlohmann::json* item_json = find_object(event, "item");
        if (!item_json) return;   // lifecycle event, no payload
        const std::string item_type = get_string(*item_json, "type");
        if (item_type != "message" && item_type != "reasoning" &&
            item_type != "function_call") {
            return;   // ignored/unknown kinds never reach a Marker anyway
        }

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
        if (type == "response.output_item.done") {
            // The completed item is the authoritative version (the
            // `added` reasoning item may lack encrypted_content) —
            // capture it whole for round-tripping in later requests.
            item.done_item = *item_json;
        }
        return;
    }

    // ---- response lifecycle ----------------------------------------------------

    if (type == "response.queued" || type == "response.created" ||
        type == "response.in_progress" ||
        type == "response.completed" || type == "response.incomplete" ||
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
        _set_terminal_status(type);
        return;
    }

    // Everything else (part boundaries, placeholders, unknown names)
    // carries nothing this layer accumulates.
}

void ResponsesReader::_assemble() {
    // Order the items by their wire output_index (parallel calls keep call
    // order, texts concatenate in production order), with slot-creation
    // order as the deterministic tie-break — orphan items without lifecycle
    // events all carry their delta-side output_index when the server sent
    // one, and 0 otherwise, where arrival order is all that is left.
    std::vector<const ItemState*> ordered;
    ordered.reserve(_items.size());
    for (const auto& [id, item] : _items) ordered.push_back(&item);
    std::sort(ordered.begin(), ordered.end(),
              [](const ItemState* a, const ItemState* b) {
                  if (a->output_index != b->output_index) {
                      return a->output_index < b->output_index;
                  }
                  return a->arrival < b->arrival;
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
