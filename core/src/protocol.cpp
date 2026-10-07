#include "core/protocol.hpp"
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace core {
void validate_session_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || id == "." || id == ".."
        || id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
        throw std::invalid_argument("session ID must contain 1..128 ASCII letters, digits, underscores or hyphens");
}
const char* operation_name(InputOperation operation) {
    switch (operation) {
        case InputOperation::Message: return "message";
        case InputOperation::Continue: return "continue";
        case InputOperation::Compact: return "compact";
    }
    throw std::invalid_argument("invalid input operation");
}

Input parse_input(const nlohmann::json& payload) {
    if (!payload.is_object()) throw std::invalid_argument("payload must be an object");
    Input input;
    input.request_id = payload.at("request_id").get<std::string>();
    if (input.request_id.empty() || input.request_id.size() > 128)
        throw std::invalid_argument("request_id must contain 1..128 bytes");
    const auto operation = payload.at("operation").get<std::string>();
    if (operation == "message") {
        input.operation = InputOperation::Message;
    } else if (operation == "continue") {
        input.operation = InputOperation::Continue;
    } else if (operation == "compact") {
        input.operation = InputOperation::Compact;
    } else {
        throw std::invalid_argument("operation must be message, continue, or compact");
    }
    for (const char* field : {"role", "invokes", "invoke_return", "type"})
        if (payload.contains(field)) throw std::invalid_argument("payload cannot contain message metadata");
    input.has_message = operation == "message";
    input.message.role = "user";
    if (input.has_message) {
        if (payload.contains("text")) {
            throw std::invalid_argument("message requires content instead of text");
        }
        const auto& parts = payload.at("content");
        if (!parts.is_array() || parts.empty()) {
            throw std::invalid_argument("content must be a nonempty array");
        }
        input.message.content.reserve(parts.size());
        for (const auto& part : parts) {
            if (!part.is_object()) {
                throw std::invalid_argument("each content part must be an object");
            }
            model_io::Content content;
            const auto type = part.at("type").get<std::string>();
            // Content's general JSON decoder tolerates unknown enum labels.
            // The public input boundary must reject them rather than silently
            // turn an attachment into text.
            if (type == "text") {
                content.type = model_io::ContentType::Text;
            } else if (type == "binary") {
                content.type = model_io::ContentType::Binary;
            } else if (type == "external_ref") {
                content.type = model_io::ContentType::ExternalRef;
            } else {
                throw std::invalid_argument("unknown content type");
            }
            // The media category is required and must be one of the five
            // labels: the boundary never infers it from the encoding, and it
            // never lets an unknown label through as if it were text.
            const auto modality = part.at("modality").get<std::string>();
            if (modality == "text") {
                content.modality = model_io::Modality::Text;
            } else if (modality == "image") {
                content.modality = model_io::Modality::Image;
            } else if (modality == "audio") {
                content.modality = model_io::Modality::Audio;
            } else if (modality == "video") {
                content.modality = model_io::Modality::Video;
            } else if (modality == "document") {
                content.modality = model_io::Modality::Document;
            } else {
                throw std::invalid_argument("unknown content modality");
            }
            content.raw = part.at("raw").get<std::string>();
            if (content.raw.empty()) {
                throw std::invalid_argument("content raw must not be empty");
            }
            const auto extras = part.find("extras");
            if (extras != part.end() && !extras->is_object()) {
                throw std::invalid_argument("content extras must be an object");
            }
            if (extras != part.end()) {
                content.extras = *extras;
            }
            input.message.content.push_back(std::move(content));
        }
    } else if (payload.contains("content") || payload.contains("text")) {
        throw std::invalid_argument(operation + " cannot contain content or text");
    }
    if (const auto options = payload.find("options"); options != payload.end()) {
        if (!options->is_object()) {
            throw InputOptionsError("options must be an object");
        }
        // Validate every category before any provider is allowed to mutate state.
        // Future category handlers belong at the same serialized admission point.
        for (const auto& [category, values] : options->items()) {
            if (!values.is_object()) {
                throw InputOptionsError("each options category must be an object");
            }
            if (category == "model" || category == "confirmation") {
                continue;
            }
            if (category != "tools" || !values.empty()) {
                throw InputOptionsError("unsupported options category: " + category);
            }
        }
        input.options = *options;
    }
    return input;
}

HistoryRequest parse_history_request(const nlohmann::json& payload) {
    if (!payload.is_object() || payload.value("operation", nlohmann::json()) != "history")
        throw std::invalid_argument("history request must be an object with operation history");
    HistoryRequest request;
    request.request_id = payload.at("request_id").get<std::string>();
    if (request.request_id.empty() || request.request_id.size() > 128)
        throw std::invalid_argument("request_id must contain 1..128 bytes");
    for (const char* field : {"content", "options", "text", "role", "invokes",
                              "invoke_return", "type"}) {
        if (!payload.contains(field)) continue;
        throw std::invalid_argument("history request cannot carry message content, options, or metadata");
    }
    const auto index = [&](const char* key, std::size_t fallback) {
        if (!payload.contains(key)) return fallback;
        const auto& value = payload.at(key);
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number <= std::numeric_limits<std::size_t>::max())
                return static_cast<std::size_t>(number);
        } else if (value.is_number_integer()) {
            const auto number = value.get<std::int64_t>();
            if (number >= 0 && static_cast<std::uint64_t>(number)
                    <= std::numeric_limits<std::size_t>::max())
                return static_cast<std::size_t>(number);
        }
        throw std::invalid_argument(std::string("history ") + key
            + " must be a nonnegative integer");
    };
    request.start = index("start", 0);
    request.step = index("step", 0);
    request.limit = index("limit", 10);
    if (request.limit == 0 || request.limit > 10)
        throw std::invalid_argument("history limit must be between 1 and 10");
    return request;
}

namespace {
std::string clipped(const std::string& raw, std::size_t maximum) {
    if (raw.size() <= maximum) return raw;
    auto end = maximum;
    while (end > 0 && (static_cast<unsigned char>(raw[end]) & 0xc0) == 0x80) --end;
    return raw.substr(0, end);
}

nlohmann::json display_content(const model_io::Content& part) {
    // The part's encoding and its media category are both projected: the panel
    // reads `type` for how `raw` is encoded and `modality` for what it is.
    nlohmann::json value = {{"type", part.type}, {"modality", part.modality}};
    if (part.type == model_io::ContentType::Binary) {
        value["raw"] = "";
        value["omitted"] = true;
        value["bytes"] = part.raw.size();
    } else {
        // The display budget follows the category: a text part may use twice
        // the room of an attachment reference.
        const auto maximum =
            part.modality == model_io::Modality::Text ? 4096u : 2048u;
        value["raw"] = clipped(part.raw, maximum);
        if (part.raw.size() > maximum) value["truncated"] = true;
    }
    return value;
}

nlohmann::json display_parts(const std::vector<model_io::Content>& parts) {
    auto result = nlohmann::json::array();
    for (std::size_t index = 0; index < std::min<std::size_t>(parts.size(), 4); ++index)
        result.push_back(display_content(parts[index]));
    return result;
}

/** Project one indivisible display step; tool payloads never enter the page. */
nlohmann::json display_step(const model_io::MessageItem& response,
                            std::size_t index) {
    nlohmann::json step = {{"index", index},
        {"content", display_parts(response.content)},
        {"tool_calls", response.invokes ? response.invokes->size() : 0},
        {"omitted_parts", response.content.size()
            - std::min<std::size_t>(response.content.size(), 4)}};
    if (response.reasoning) step["reasoning"] = display_content(*response.reasoning);
    return step;
}
} // namespace

nlohmann::json history_page(const model_io::AgentInputState& state,
                            const HistoryRequest& request,
                            std::uint64_t revision) {
    if (request.start >= state.turns.size() && request.step != 0)
        throw std::invalid_argument("history step is outside the available turns");
    const auto start = std::min(request.start, state.turns.size());
    const auto end = start + std::min(request.limit, state.turns.size() - start);
    // Reserve the widest possible returned cursors. Final cursor values and
    // omission counts can only make this conservative serialized total smaller.
    // Measure each projected object once, rather than repeatedly serializing
    // the growing page (which would be quadratic for many small steps).
    nlohmann::json page = {{"request_id", request.request_id}, {"revision", revision},
        {"start", start}, {"step", request.step}, {"next", state.turns.size()},
        {"next_step", std::numeric_limits<std::size_t>::max()},
        {"total", state.turns.size()}, {"turns", nlohmann::json::array()}};
    auto& turns = page["turns"];
    auto page_bytes = page.dump().size();
    if (page_bytes > history_page_max_bytes)
        throw std::length_error("history page metadata exceeds the display budget");
    auto next = start;
    std::size_t next_step = 0;
    for (auto index = start; index < end; ++index) {
        const auto& turn = state.turns[index];
        const auto first_step = index == start ? request.step : 0;
        if (first_step > turn.agent_loop_step.size())
            throw std::invalid_argument("history step is outside the selected turn");
        nlohmann::json projected = {{"index", index},
            {"user", display_parts(turn.user_input.content)},
            {"steps", nlohmann::json::array()},
            {"omitted_user_parts", turn.user_input.content.size()
                - std::min<std::size_t>(turn.user_input.content.size(), 4)},
            {"omitted_steps", turn.agent_loop_step.size() - first_step}};
        // Only host-owned MessageItem metadata can hide an internal input.
        // Content extras supplied by a user never participate in this check.
        const auto& metadata = turn.user_input.extras;
        if (metadata && metadata->is_object()
            && metadata->value("simplex.internal_input", "") == "auto_compact_continue") {
            projected["user"] = nlohmann::json::array();
            projected["omitted_user_parts"] = 0;
            projected["internal_input"] = "auto_compact_continue";
            if (metadata->contains("simplex.source")) {
                projected["source"] = metadata->at("simplex.source");
            }
        }
        auto& steps = projected["steps"];
        auto turn_bytes = projected.dump().size();
        const auto separator_bytes = turns.empty() ? 0u : 1u;
        if (page_bytes + separator_bytes + turn_bytes > history_page_max_bytes) {
            if (turns.empty())
                throw std::length_error("history turn exceeds the display budget");
            break;
        }
        std::size_t step_index = first_step;
        for (; step_index < turn.agent_loop_step.size(); ++step_index) {
            auto step = display_step(
                turn.agent_loop_step[step_index].model_response, step_index);
            const auto commit = turn.agent_loop_step[step_index].commit_sequence;
            if (commit != 0) step["commit_sequence"] = std::to_string(commit);
            const auto bytes = step.dump().size() + (steps.empty() ? 0u : 1u);
            if (page_bytes + separator_bytes + turn_bytes + bytes
                    > history_page_max_bytes) break;
            turn_bytes += bytes;
            steps.push_back(std::move(step));
        }
        if (step_index == first_step && step_index < turn.agent_loop_step.size()) {
            // Do not publish a new turn without its first remaining step: the
            // current cursor cannot express that boundary without stalling.
            if (turns.empty())
                throw std::length_error("history step exceeds the display budget");
            break;
        }
        projected["omitted_steps"] = turn.agent_loop_step.size() - step_index;
        page_bytes += separator_bytes + turn_bytes;
        turns.push_back(std::move(projected));
        if (step_index < turn.agent_loop_step.size()) {
            next = index;
            next_step = step_index;
            break;
        }
        next = index + 1;
    }
    page["next"] = next;
    page["next_step"] = next_step;
    return page;
}
std::string new_identity() {
    return boost::uuids::to_string(boost::uuids::random_generator()());
}
} // namespace core
