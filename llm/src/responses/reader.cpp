// responses/reader.cpp — the delta accumulation + assembly of the
// Responses-API layer. Fed exclusively by the delta stream the decoder
// guarantees lossless (one delta per event, full event JSON on markers), so
// every fold here is exception-free bookkeeping: guarded nlohmann access
// only, never throws — a throw is a reader bug that next() escalates to a
// stream fault (finish(ERROR)) after the fact.

#include "llm/responses/reader.hpp"

#include <algorithm>
#include <utility>

#include "llm/responses/event_access.hpp"

namespace llm::responses {

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

std::string join_content_parts(
    const std::map<std::size_t, std::string>& parts) {
    std::string joined;
    for (const auto& [index, text] : parts) joined += text;
    return joined;
}

std::optional<std::uint64_t> get_uint64(
    const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return std::nullopt;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_unsigned()) return std::nullopt;
    return it->get<std::uint64_t>();
}

} // namespace

ResponsesReader::ItemState& ResponsesReader::_item(
    const std::string& id, std::optional<std::size_t> output_index) {
    // Deltas may legitimately arrive before output_item.added (or without it
    // ever coming): open an untyped slot rather than losing the bytes.
    const std::string key = !id.empty()
        ? id
        : output_index
            ? std::string("@output:") + std::to_string(*output_index)
            : std::string("@orphan");
    ItemState& slot = _items[key];
    if (slot.arrival == ItemState::kUnstamped) {
        // First touch: record the creation order _assemble uses to break
        // output_index ties.
        slot.arrival = _next_arrival++;
    }
    if (output_index) slot.output_index = *output_index;
    return slot;
}

void ResponsesReader::_accumulate_output_item(
    const nlohmann::json& item_json,
    std::optional<std::size_t> output_index,
    bool authoritative) {
    if (!item_json.is_object()) return;
    ItemState& item = _item(get_string(item_json, "id"), output_index);
    const std::string item_type = get_string(item_json, "type");
    if (!item_type.empty()) item.type = item_type;

    if (item_type == "message") {
        const auto content = item_json.find("content");
        if (content != item_json.end() && content->is_array()) {
            for (std::size_t index = 0; index < content->size(); ++index) {
                const auto& part = (*content)[index];
                const std::string part_type = get_string(part, "type");
                if (part_type == "output_text") {
                    const std::string text = get_string(part, "text");
                    if (authoritative || !text.empty()) item.text_parts[index] = text;
                } else if (part_type == "refusal") {
                    const std::string refusal = get_string(part, "refusal");
                    if (authoritative || !refusal.empty()) {
                        item.refusal_parts[index] = refusal;
                    }
                }
            }
        }
    } else if (item_type == "reasoning") {
        const auto summary = item_json.find("summary");
        if (summary != item_json.end() && summary->is_array()) {
            for (std::size_t index = 0; index < summary->size(); ++index) {
                const std::string text = get_string((*summary)[index], "text");
                if (authoritative || !text.empty()) item.summary_parts[index] = text;
            }
        }
        const auto content = item_json.find("content");
        if (content != item_json.end() && content->is_array()) {
            for (std::size_t index = 0; index < content->size(); ++index) {
                const std::string text = get_string((*content)[index], "text");
                if (authoritative || !text.empty()) item.text_parts[index] = text;
            }
        }
    } else if (item_type == "function_call") {
        const std::string call_id = get_string(item_json, "call_id");
        const std::string name = get_string(item_json, "name");
        const std::string arguments = get_string(item_json, "arguments");
        if (authoritative || !call_id.empty()) item.call_id = call_id;
        if (authoritative || !name.empty()) item.name = name;
        if (authoritative || !arguments.empty()) item.arguments = arguments;
    }

    if (authoritative) item.done_item = item_json;
}

void ResponsesReader::_accumulate(const ResponsesDelta& delta) {
    switch (delta.kind) {
        // ---- increments: append their bytes ------------------------------
        // The delta's output_index (when the event carried one) keeps even
        // lifecycle-less slots ordered: it is the only ordering signal an
        // orphan item ever gets.
        case DeltaKind::Text:
        case DeltaKind::ReasoningText: {
            ItemState& item = _item(delta.item_id, delta.output_index);
            item.text_parts[delta.content_index.value_or(0)] += delta.text;
            return;
        }
        case DeltaKind::Refusal: {
            ItemState& item = _item(delta.item_id, delta.output_index);
            item.refusal_parts[delta.content_index.value_or(0)] += delta.text;
            return;
        }
        case DeltaKind::ToolCallArgs: {
            ItemState& item = _item(delta.item_id, delta.output_index);
            item.arguments += delta.text;
            return;
        }
        case DeltaKind::ReasoningSummary: {
            ItemState& item = _item(delta.item_id, delta.output_index);
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
    else if (type == "response.cancelled" || type == "cancelled") {
        _status = StreamStatus::Cancelled;
    }
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
        ItemState& item = _item(get_string(event, "item_id"),
                                get_index(event, "output_index"));
        std::string full = get_string(event, "text");
        if (!full.empty()) {
            item.text_parts[get_index(event, "content_index").value_or(0)] =
                std::move(full);
        }
        return;
    }
    if (type == "response.refusal.done") {
        ItemState& item = _item(get_string(event, "item_id"),
                                get_index(event, "output_index"));
        std::string full = get_string(event, "refusal");
        if (!full.empty()) {
            item.refusal_parts[get_index(event, "content_index").value_or(0)] =
                std::move(full);
        }
        return;
    }
    if (type == "response.reasoning_text.done") {
        ItemState& item = _item(get_string(event, "item_id"),
                                get_index(event, "output_index"));
        std::string full = get_string(event, "text");
        if (!full.empty()) {
            item.text_parts[get_index(event, "content_index").value_or(0)] =
                std::move(full);
        }
        return;
    }
    if (type == "response.reasoning_summary_text.done") {
        // Authoritative for ITS part only; other parts keep their own text.
        ItemState& item = _item(get_string(event, "item_id"),
                                get_index(event, "output_index"));
        std::string full = get_string(event, "text");
        if (!full.empty()) {
            item.summary_parts[summary_part_index(event)] = std::move(full);
        }
        return;
    }
    if (type == "response.function_call_arguments.done") {
        ItemState& item = _item(get_string(event, "item_id"),
                                get_index(event, "output_index"));
        std::string arguments = get_string(event, "arguments");
        if (!arguments.empty()) item.arguments = std::move(arguments);
        std::string name = get_string(event, "name");
        if (!name.empty()) item.name = std::move(name);
        return;
    }
    if (type == "response.reasoning_summary_part.done") {
        // The part lifecycle's done event carries the part's authoritative
        // text; capture it into its indexed slot.
        ItemState& item = _item(get_string(event, "item_id"),
                                get_index(event, "output_index"));
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
        _accumulate_output_item(*item_json, get_index(event, "output_index"),
                                type == "response.output_item.done");
        return;
    }

    // ---- response lifecycle ----------------------------------------------------

    if (type == "response.queued" || type == "response.created" ||
        type == "response.in_progress" ||
        type == "response.completed" || type == "response.incomplete" ||
        type == "response.failed" || type == "response.cancelled" ||
        type == "error") {
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
            if (const auto output = response->find("output");
                output != response->end() && output->is_array()) {
                for (std::size_t index = 0; index < output->size(); ++index) {
                    _accumulate_output_item((*output)[index], index, true);
                }
            }
            const std::string response_status = get_string(*response, "status");
            if (response_status == "completed") _status = StreamStatus::Completed;
            else if (response_status == "incomplete") _status = StreamStatus::Incomplete;
            else if (response_status == "failed") _status = StreamStatus::Failed;
            else if (response_status == "cancelled") _status = StreamStatus::Cancelled;
        }
        if (type == "error") {
            // A bare error event IS the error record (code/message/param).
            _terminal_details = event;
        }
        if (_status == StreamStatus::Streaming) _set_terminal_status(type);
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
        text += join_content_parts(item.text_parts);
        refusal += join_content_parts(item.refusal_parts);
    };
    const auto as_reasoning = [&](const ItemState& item) {
        reasoning_text += join_content_parts(item.text_parts);
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
            } else if (!join_content_parts(item->text_parts).empty() ||
                       !join_content_parts(item->refusal_parts).empty()) {
                as_message(*item);
            }
        }
        // Every completed item — handled kind or not — is recorded for
        // round-tripping; the interpreter re-emits the kinds it maps.
        if (item->done_item.is_object()) output_items.push_back(item->done_item);
    }

    model_io::Content content;
    content.type = model_io::ContentType::Text;
    content.raw = std::move(text);
    if (!refusal.empty()) {
        // Refusal is a distinct part kind: keep it out of the visible text
        // when there is any; only a refusal-only response surfaces it there.
        if (content.raw.empty()) {
            content.raw = std::move(refusal);
        } else {
            content.extras = nlohmann::json{{"refusal", std::move(refusal)}};
        }
    }
    result.content.push_back(std::move(content));

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

    if (_usage && _usage->is_object()) {
        model_io::TokenCost cost;
        bool has_cost = false;
        if (auto value = get_uint64(*_usage, "input_tokens")) {
            cost.prompt = *value;
            has_cost = true;
        }
        if (auto value = get_uint64(*_usage, "output_tokens")) {
            cost.generated = *value;
            has_cost = true;
        }
        if (const auto details = _usage->find("input_tokens_details");
            details != _usage->end() && details->is_object()) {
            if (auto value = get_uint64(*details, "cached_tokens")) {
                cost.cache_hit = *value;
                has_cost = true;
            }
        }
        if (has_cost) result.cost = cost;
    }

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

} // namespace llm::responses
