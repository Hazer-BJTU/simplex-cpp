#include "core/protocol.hpp"
#include "core/wire.hpp"
#include "protocol/wire.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <string_view>
#include <openssl/evp.h>

namespace core {
namespace {
using Json = nlohmann::json;

/** UTF-8 prefix of a validated model string; never split a code point. */
std::string prefix(const std::string& text, std::size_t count) {
    auto end = std::min(text.size(), count);
    while (end > 0 && end < text.size()
        && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

/** Bound tree traversal as well as the resulting diagnostic representation. */
Json preview(const Json& value, std::size_t depth, std::size_t& remaining) {
    if (remaining == 0 || depth > 12) {
        return Json{{"omitted", true}};
    }
    --remaining;
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() <= 1024) return text;
        return prefix(text, 1024);
    }
    if (value.is_array()) {
        auto result = Json::array();
        for (const auto& item : value) {
            if (result.size() >= 64 || remaining == 0) break;
            result.push_back(preview(item, depth + 1, remaining));
        }
        if (result.size() < value.size()) {
            result.push_back(Json{{"omitted_items", value.size() - result.size()}});
        }
        return result;
    }
    if (value.is_object()) {
        auto result = Json::object();
        for (const auto& [key, item] : value.items()) {
            if (result.size() >= 64 || remaining == 0) break;
            if (key.size() > 128) continue;
            result[key] = preview(item, depth + 1, remaining);
            if (item.is_string() && item.get_ref<const std::string&>().size() > 1024) {
                result["display_truncated"] = true;
                if (key == "raw") {
                    result["truncated"] = true;
                    result["bytes"] = item.get_ref<const std::string&>().size();
                }
            }
        }
        if (result.size() < value.size()) result["display_truncated"] = true;
        return result;
    }
    return value;
}

/** Independent bounded diagnostics; oversized trees become explicit omissions. */
Json bounded_preview(const Json& value, std::size_t maximum) {
    std::size_t remaining = 128;
    auto result = preview(value, 0, remaining);
    if (result.dump().size() > maximum) {
        return Json{{"display_omitted", true}};
    }
    return result;
}

/** Account for JSON escaping while preserving a valid UTF-8 string prefix. */
std::string encoded_prefix(const std::string& text, std::size_t maximum) {
    std::size_t low = 0;
    std::size_t high = std::min(text.size(), maximum - 2);
    const auto candidate = prefix(text, high);
    if (Json(candidate).dump().size() <= maximum) {
        return candidate;
    }
    while (low < high) {
        const auto middle = low + (high - low + 1) / 2;
        if (Json(prefix(text, middle)).dump().size() <= maximum) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return prefix(text, low);
}

/** Keep outcome/error annotations even when unrelated metadata exhausts its budget. */
Json annotations(const Json& value) {
    auto result = bounded_preview(value, 512);
    if (!value.is_object()) {
        return result;
    }
    for (const auto* key : {"status", "loop_skipped"}) {
        const auto item = value.find(key);
        if (item != value.end() && item->is_primitive()) {
            result[key] = bounded_preview(*item, 256);
        }
    }
    const auto error = value.find("error");
    if (error != value.end() && error->is_object()) {
        auto projected = Json::object();
        for (const auto* key : {"stage", "message"}) {
            const auto item = error->find(key);
            if (item == error->end() || !item->is_string()) {
                continue;
            }
            const auto& original = item->get_ref<const std::string&>();
            const auto maximum = std::string_view(key) == "stage" ? 128 : 1024;
            const auto text = encoded_prefix(original, maximum);
            projected[key] = text;
            if (text != original) {
                projected["display_truncated"] = true;
            }
        }
        result["error"] = std::move(projected);
    }
    return result;
}

/** Identity/classification are copied before independent arguments and extras. */
protocol::Call call_preview(const model_io::InvokeQuery& call, std::size_t argument_budget) {
    protocol::Call result;
    result.id = call.id;
    result.name = call.name;
    result.type = wire_scheduling(call.type);
    result.security = wire_security(call.security);
    result.arguments = bounded_preview(call.arguments, argument_budget);
    if (call.extras) {
        result.extras = annotations(*call.extras);
    }
    return result;
}

/** Output text and its labels cannot be displaced by nested output metadata. */
protocol::Content tool_content(const model_io::Content& content, std::size_t text_budget) {
    protocol::Content result;
    result.type = wire_type(content.type);
    result.modality = wire_modality(content.modality);
    result.raw = encoded_prefix(content.raw, text_budget);
    if (result.raw != content.raw) {
        result.truncated = true;
        result.bytes = content.raw.size();
    }
    if (content.extras) {
        result.extras = bounded_preview(*content.extras, 512);
    }
    return result;
}

/** History parts keep their category budget and mark binary parts as omitted. */
protocol::Content display_content(const model_io::Content& part,
                                  std::size_t maximum = 4096) {
    protocol::Content result;
    result.type = wire_type(part.type);
    result.modality = wire_modality(part.modality);
    if (part.type == model_io::ContentType::Binary) {
        result.raw.clear();
        result.omitted = true;
        result.bytes = part.raw.size();
        return result;
    }
    // The display budget follows the category: a text part may use twice the
    // room of an attachment reference.
    if (part.modality != model_io::Modality::Text) maximum = std::min<std::size_t>(maximum, 2048);
    result.raw = prefix(part.raw, maximum);
    if (part.raw.size() > maximum) {
        result.truncated = true;
        result.bytes = part.raw.size();
    }
    return result;
}

/** The part's encoding and media category are both projected, never inferred. */
std::vector<protocol::Content> display_parts(const std::vector<model_io::Content>& parts) {
    std::vector<protocol::Content> result;
    for (std::size_t index = 0; index < std::min<std::size_t>(parts.size(), 4); ++index) {
        result.push_back(display_content(parts[index]));
    }
    return result;
}

/** Answer preview budget counts escaped JSON across all parts, not each field. */
protocol::HistoryStep display_step(const model_io::MessageItem& response,
                                   std::size_t index, std::size_t answer_budget = 96 * 1024) {
    protocol::HistoryStep step;
    step.index = index;
    std::size_t remaining = answer_budget;
    for (const auto& part : response.content) {
        if (step.content.size() >= 128 || remaining < 1024) break;
        // Probe only a bounded prefix. Measure actual JSON escaping rather
        // than charging every character the worst-case six bytes.
        auto maximum = std::min<std::size_t>(part.raw.size(), remaining - 256);
        auto projected = display_content(part, maximum);
        if (protocol::to_json(projected).dump().size() > remaining) {
            std::size_t low = 0;
            std::size_t high = maximum;
            while (low < high) {
                const auto middle = low + (high - low + 1) / 2;
                if (protocol::to_json(display_content(part, middle)).dump().size() <= remaining) {
                    low = middle;
                } else {
                    high = middle - 1;
                }
            }
            projected = display_content(part, low);
        }
        const auto bytes = protocol::to_json(projected).dump().size();
        if (bytes > remaining) break;
        remaining -= bytes;
        step.content.push_back(std::move(projected));
    }
    step.tool_calls = response.invokes ? response.invokes->size() : 0;
    step.omitted_parts = response.content.size() - step.content.size();
    if (response.reasoning) step.reasoning = display_content(*response.reasoning, 4096);
    return step;
}

/** One committed response preview without the projection-only annotations. */
protocol::ResponseProjection preview_projection(const model_io::MessageItem& response,
                                                std::size_t step, std::size_t answer_budget) {
    const auto projected = display_step(response, step, answer_budget);
    protocol::ResponseProjection result;
    result.index = projected.index;
    result.content = projected.content;
    result.tool_calls = projected.tool_calls;
    result.omitted_parts = projected.omitted_parts;
    result.reasoning = projected.reasoning;
    return result;
}

/** Hash original answer bytes without copying them. Metadata/reasoning are not answer content. */
std::string fingerprint(const model_io::MessageItem& response) {
    const auto release = [](EVP_MD_CTX* context) { EVP_MD_CTX_free(context); };
    std::unique_ptr<EVP_MD_CTX, decltype(release)> context(EVP_MD_CTX_new(), release);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("cannot initialize answer fingerprint");
    }
    for (const auto& part : response.content) {
        const auto identity = Json{{"type", part.type}, {"modality", part.modality},
            {"bytes", part.raw.size()}}.dump();
        if (EVP_DigestUpdate(context.get(), identity.data(), identity.size()) != 1
            || EVP_DigestUpdate(context.get(), part.raw.data(), part.raw.size()) != 1) {
            throw std::runtime_error("cannot compute answer fingerprint");
        }
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest, &size) != 1) {
        throw std::runtime_error("cannot finish answer fingerprint");
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(size * 2);
    for (unsigned int index = 0; index < size; ++index) {
        text.push_back(hex[digest[index] >> 4]);
        text.push_back(hex[digest[index] & 15]);
    }
    return text;
}

/** Exact-answer locator for one committed response. */
protocol::AnswerSource source(const model_io::AgentLoopStep& response,
                              std::size_t turn, std::size_t step, const std::string& worker_id) {
    protocol::AnswerSource result;
    result.worker_id = worker_id;
    result.turn = turn;
    result.step = step;
    result.commit_sequence = std::to_string(response.commit_sequence);
    result.fingerprint = fingerprint(response.model_response);
    return result;
}
} // namespace

Json display_value(const Json& value) {
    std::size_t remaining = 128;
    return preview(value, 0, remaining);
}

Json display_compact(Json value) {
    if (!value.is_object() || !value.contains("summary")) {
        return display_value(value);
    }
    auto summary = std::move(value.at("summary"));
    value.erase("summary");
    auto result = display_value(value);
    if (!summary.is_string()) {
        result["summary"] = display_value(summary);
    } else if (summary.get_ref<const std::string&>().size() > compact_summary_max_bytes) {
        result["summary"] = Json{
            {"display_omitted", true},
            {"bytes", summary.get_ref<const std::string&>().size()},
            {"reason", "compact summary exceeds 32768 byte limit"}
        };
    } else {
        result["summary"] = std::move(summary);
    }
    return result;
}

Json display_calls(const std::vector<model_io::InvokeQuery>& calls) {
    Json result = Json::array();
    const auto count = std::min<std::size_t>(calls.size(), 64);
    const auto argument_budget = std::min<std::size_t>(8192, 64 * 1024 / std::max<std::size_t>(count, 1));
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(protocol::to_json(call_preview(calls[index], argument_budget)));
    }
    if (count < calls.size()) {
        protocol::OmissionMarker omitted;
        omitted.omitted_items = calls.size() - count;
        result.push_back(protocol::to_json(omitted));
    }
    return result;
}

Json display_results(const std::vector<model_io::MessageItem>& messages) {
    Json result = Json::array();
    const auto count = std::min<std::size_t>(messages.size(), 64);
    const auto argument_budget = std::min<std::size_t>(8192, 64 * 1024 / std::max<std::size_t>(count, 1));
    const auto output_budget = std::min<std::size_t>(8192, 128 * 1024 / std::max<std::size_t>(count, 1));
    for (std::size_t index = 0; index < count; ++index) {
        const auto& message = messages[index];
        protocol::ToolResultMessage entry;
        entry.type = wire_type(message.type);
        entry.role = message.role;
        const auto parts = std::min<std::size_t>(message.content.size(), 4);
        for (std::size_t part = 0; part < parts; ++part) {
            entry.content.push_back(tool_content(message.content[part], output_budget / parts));
        }
        if (parts < message.content.size()) {
            entry.omitted_parts = message.content.size() - parts;
        }
        if (message.invoke_return) {
            const auto& record = *message.invoke_return;
            protocol::Result provenance;
            provenance.query = call_preview(record.query, argument_budget);
            provenance.output = tool_content(record.output, output_budget);
            if (record.extras) {
                provenance.extras = annotations(*record.extras);
            }
            entry.invoke_return = std::move(provenance);
        }
        if (message.extras) {
            entry.extras = annotations(*message.extras);
        }
        result.push_back(protocol::to_json(entry));
    }
    if (count < messages.size()) {
        protocol::OmissionMarker omitted;
        omitted.omitted_items = messages.size() - count;
        result.push_back(protocol::to_json(omitted));
    }
    return result;
}

Json answer_source(const model_io::AgentLoopStep& response,
    std::size_t turn, std::size_t step, const std::string& worker_id) {
    return protocol::to_json(source(response, turn, step, worker_id));
}

Json project_response(const model_io::AgentInputState& state,
    std::size_t turn, std::size_t step, const std::string& worker_id) {
    const auto& committed = state.turns.at(turn).agent_loop_step.at(step);
    const auto& response = committed.model_response;
    auto result = preview_projection(response, step, 512 * 1024);
    if (committed.commit_sequence != 0) {
        result.commit_sequence = std::to_string(committed.commit_sequence);
        result.answer_source = source(committed, turn, step, worker_id);
    }
    result.type = wire_type(response.type);
    result.role = response.role;
    if (response.cost) {
        result.cost = protocol::Usage{response.cost->prompt, response.cost->generated,
                                      response.cost->cache_hit};
    }
    if (response.invokes) result.invokes = display_calls(*response.invokes);
    return protocol::to_json(result);
}

Json history_page(const model_io::AgentInputState& state,
                  const HistoryRequest& request,
                  std::uint64_t revision,
                  const std::string& worker_id) {
    if (request.start >= state.turns.size() && request.step != 0)
        throw std::invalid_argument("history step is outside the available turns");
    const auto start = std::min(request.start, state.turns.size());
    const auto end = start + std::min(request.limit, state.turns.size() - start);
    // Reserve the widest possible returned cursors. Final cursor values and
    // omission counts can only make this conservative serialized total smaller.
    // Measure each projected object once, rather than repeatedly serializing
    // the growing page (which would be quadratic for many small steps).
    protocol::HistoryPage page;
    page.request_id = request.request_id;
    page.revision = revision;
    page.start = start;
    page.step = request.step;
    page.next = state.turns.size();
    page.next_step = std::numeric_limits<std::size_t>::max();
    page.total = state.turns.size();
    auto page_bytes = protocol::to_json(page).dump().size();
    if (page_bytes > history_page_max_bytes)
        throw std::length_error("history page metadata exceeds the display budget");
    auto next = start;
    std::size_t next_step = 0;
    for (auto index = start; index < end; ++index) {
        const auto& turn = state.turns[index];
        const auto first_step = index == start ? request.step : 0;
        if (first_step > turn.agent_loop_step.size())
            throw std::invalid_argument("history step is outside the selected turn");
        protocol::HistoryTurn projected;
        projected.index = index;
        projected.user = display_parts(turn.user_input.content);
        projected.omitted_user_parts = turn.user_input.content.size()
            - std::min<std::size_t>(turn.user_input.content.size(), 4);
        projected.omitted_steps = turn.agent_loop_step.size() - first_step;
        // Only host-owned MessageItem metadata can hide an internal input.
        // Content extras supplied by a user never participate in this check.
        const auto& metadata = turn.user_input.extras;
        if (metadata && metadata->is_object() && metadata->contains("simplex.source")) {
            projected.source = protocol::from_json<protocol::ExecutionIdentity>(
                metadata->at("simplex.source"), "source");
        }
        if (metadata && metadata->is_object()
            && metadata->value("simplex.internal_input", "") == "auto_compact_continue") {
            projected.user.clear();
            projected.omitted_user_parts = 0;
            projected.internal_input = "auto_compact_continue";
        }
        auto turn_bytes = protocol::to_json(projected).dump().size();
        const auto separator_bytes = page.turns.empty() ? 0u : 1u;
        if (page_bytes + separator_bytes + turn_bytes > history_page_max_bytes) {
            if (page.turns.empty())
                throw std::length_error("history turn exceeds the display budget");
            break;
        }
        std::size_t step_index = first_step;
        for (; step_index < turn.agent_loop_step.size(); ++step_index) {
            auto step = display_step(
                turn.agent_loop_step[step_index].model_response, step_index);
            const auto commit = turn.agent_loop_step[step_index].commit_sequence;
            if (commit != 0) {
                step.commit_sequence = std::to_string(commit);
                if (!worker_id.empty()) step.answer_source = source(
                    turn.agent_loop_step[step_index], index, step_index, worker_id);
            }
            const auto& execution = turn.agent_loop_step[step_index].extras;
            if (execution && execution->is_object()
                && execution->contains("simplex.execution")) {
                step.execution = protocol::from_json<protocol::ExecutionIdentity>(
                    execution->at("simplex.execution"), "execution");
            }
            const auto bytes = protocol::to_json(step).dump().size()
                + (projected.steps.empty() ? 0u : 1u);
            if (page_bytes + separator_bytes + turn_bytes + bytes
                    > history_page_max_bytes) break;
            turn_bytes += bytes;
            projected.steps.push_back(std::move(step));
        }
        if (step_index == first_step && step_index < turn.agent_loop_step.size()) {
            // Do not publish a new turn without its first remaining step: the
            // current cursor cannot express that boundary without stalling.
            if (page.turns.empty())
                throw std::length_error("history step exceeds the display budget");
            break;
        }
        projected.omitted_steps = turn.agent_loop_step.size() - step_index;
        page_bytes += separator_bytes + turn_bytes;
        page.turns.push_back(std::move(projected));
        if (step_index < turn.agent_loop_step.size()) {
            next = index;
            next_step = step_index;
            break;
        }
        next = index + 1;
    }
    page.next = next;
    page.next_step = next_step;
    return protocol::to_json(page);
}

Json answer_page(const model_io::AgentInputState& state, const Json& query,
    const std::string& worker_id) {
    protocol::PayloadRequest request;
    try {
        request = protocol::from_json<protocol::PayloadRequest>(query);
    } catch (const protocol::ValidationError& error) {
        throw std::invalid_argument(error.what());
    }
    if (request.operation != protocol::InputOperation::answer) {
        throw std::invalid_argument("expected answer query");
    }
    if (request.request_id.empty() || request.request_id.size() > 128)
        throw std::invalid_argument("invalid request_id");
    if (!request.source || request.source->worker_id != worker_id)
        throw std::invalid_argument("answer source is unavailable in this worker");
    const auto index = [](std::uint64_t value) -> std::size_t {
        if (value > std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument("answer cursor must be a nonnegative safe integer");
        return static_cast<std::size_t>(value);
    };
    const auto locator = *request.source;
    const auto turn = locator.turn;
    const auto step = locator.step;
    if (turn >= state.turns.size() || step >= state.turns[turn].agent_loop_step.size()) {
        throw std::invalid_argument("answer source expired after state replacement");
    }
    const auto& committed = state.turns[turn].agent_loop_step[step];
    if (locator.commit_sequence != std::to_string(committed.commit_sequence)
        || committed.commit_sequence == 0) {
        throw std::invalid_argument("answer commit identity does not match");
    }
    if (locator.fingerprint != fingerprint(committed.model_response)) {
        throw std::invalid_argument("answer content changed; refresh history before reading it");
    }
    const auto& parts = committed.model_response.content;
    const auto part_index = index(request.part.value_or(0));
    const auto offset = index(request.offset.value_or(0));
    if (part_index >= parts.size()) throw std::invalid_argument("answer part is unavailable");
    const auto& part = parts[part_index];
    if (part.type == model_io::ContentType::Binary) {
        throw std::invalid_argument("binary answer content is not available as text");
    }
    if (offset > part.raw.size() || (offset < part.raw.size()
        && (static_cast<unsigned char>(part.raw[offset]) & 0xc0) == 0x80)) {
        throw std::invalid_argument("answer offset is outside a UTF-8 boundary");
    }
    auto end = offset + std::min<std::size_t>(32 * 1024, part.raw.size() - offset);
    while (end > offset && end < part.raw.size()
        && (static_cast<unsigned char>(part.raw[end]) & 0xc0) == 0x80) {
        --end;
    }
    const bool finished = end == part.raw.size();
    protocol::AnswerPage page;
    page.request_id = request.request_id;
    page.source = locator;
    page.part = part_index;
    page.offset = offset;
    page.next_offset = end;
    page.bytes = part.raw.size();
    page.total_parts = parts.size();
    page.next_part = finished ? part_index + 1 : part_index;
    page.done = finished && part_index + 1 == parts.size();
    page.type = wire_type(part.type);
    page.modality = wire_modality(part.modality);
    page.raw = part.raw.substr(offset, end - offset);
    return protocol::to_json(page);
}
} // namespace core
