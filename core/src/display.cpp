#include "core/protocol.hpp"

#include <algorithm>
#include <limits>
#include <memory>
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
} // namespace

Json display_value(const Json& value) {
    std::size_t remaining = 128;
    return preview(value, 0, remaining);
}

Json answer_source(const model_io::AgentLoopStep& response,
    std::size_t turn, std::size_t step, const std::string& worker_id) {
    return {{"worker_id", worker_id}, {"turn", turn}, {"step", step},
        {"commit_sequence", std::to_string(response.commit_sequence)},
        {"fingerprint", fingerprint(response.model_response)}};
}

Json project_response(const model_io::AgentInputState& state,
    std::size_t turn, std::size_t step, const std::string& worker_id) {
    const auto& committed = state.turns.at(turn).agent_loop_step.at(step);
    const auto& response = committed.model_response;
    Json result = response_preview(response, step, 512 * 1024);
    if (committed.commit_sequence != 0) {
        result["commit_sequence"] = std::to_string(committed.commit_sequence);
        result["answer_source"] = answer_source(committed, turn, step, worker_id);
    }
    result["role"] = response.role;
    result["type"] = response.type;
    if (response.cost) result["cost"] = *response.cost;
    if (response.invokes) result["invokes"] = display_value(Json(*response.invokes));
    return result;
}

Json answer_page(const model_io::AgentInputState& state, const Json& request,
    const std::string& worker_id) {
    if (!request.is_object() || request.value("operation", Json()) != "answer") {
        throw std::invalid_argument("expected answer query");
    }
    const auto id = request.at("request_id").get<std::string>();
    if (id.empty() || id.size() > 128) throw std::invalid_argument("invalid request_id");
    for (const auto& [key, value] : request.items()) {
        if (key != "operation" && key != "request_id" && key != "source"
            && key != "part" && key != "offset") {
            throw std::invalid_argument("unexpected answer query field");
        }
    }
    const auto& source = request.at("source");
    if (!source.is_object() || source.size() != 5
        || source.at("worker_id") != worker_id) {
        throw std::invalid_argument("answer source is unavailable in this worker");
    }
    const auto index = [](const Json& value) -> std::size_t {
        if (!value.is_number_integer() || value < 0
            || value > 9007199254740991ULL) {
            throw std::invalid_argument("answer cursor must be a nonnegative safe integer");
        }
        return value.get<std::size_t>();
    };
    const auto turn = index(source.at("turn"));
    const auto step = index(source.at("step"));
    if (turn >= state.turns.size() || step >= state.turns[turn].agent_loop_step.size()) {
        throw std::invalid_argument("answer source expired after state replacement");
    }
    const auto& committed = state.turns[turn].agent_loop_step[step];
    if (source.at("commit_sequence") != std::to_string(committed.commit_sequence)
        || committed.commit_sequence == 0) {
        throw std::invalid_argument("answer commit identity does not match");
    }
    if (source.at("fingerprint") != fingerprint(committed.model_response)) {
        throw std::invalid_argument("answer content changed; refresh history before reading it");
    }
    const auto& parts = committed.model_response.content;
    const auto part_index = index(request.value("part", Json(0)));
    const auto offset = index(request.value("offset", Json(0)));
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
    return Json{{"request_id", id}, {"source", source}, {"part", part_index},
        {"offset", offset}, {"next_offset", end}, {"bytes", part.raw.size()},
        {"total_parts", parts.size()}, {"next_part", finished ? part_index + 1 : part_index},
        {"done", finished && part_index + 1 == parts.size()},
        {"type", part.type}, {"modality", part.modality},
        {"raw", part.raw.substr(offset, end - offset)}};
}
} // namespace core
