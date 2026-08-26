/**
 * @file dialect.hpp
 * @brief DeepSeek's spelling of the OpenAI-compatible Chat Completions
 *        protocol: endpoint defaults, thinking-mode request policy, and the
 *        cache-hit usage normalisation.
 *
 * DeepSeek (https://api-docs.deepseek.com) speaks the chat-completions wire
 * with a few provider deviations, each carried by exactly one hook here:
 *
 *   - endpoint: POST https://api.deepseek.com/chat/completions (no /v1
 *     prefix) with Bearer auth;
 *   - thinking mode is ON by default and toggled by `thinking`:
 *     `{"type": "enabled" | "disabled"}` — an explicit native object in the
 *     config passes through verbatim, otherwise the dialect emits it:
 *     efforts "none"/"minimal" mean disabled, anything else enabled;
 *   - `reasoning_effort` only recognises "high"/"max" (the API maps
 *     low/medium→high and xhigh→max itself; this dialect applies the same
 *     documented mapping client-side so the wire value is always in
 *     vocabulary);
 *   - thinking mode + tools requires every intermediate assistant message
 *     to replay its reasoning_content verbatim or the API answers 400 —
 *     hence replay_assistant_reasoning();
 *   - `n` is undocumented and frequency_penalty/presence_penalty are
 *     deprecated no-ops: all three are stripped;
 *   - usage reports cache hits as prompt_cache_hit_tokens (not OpenAI's
 *     prompt_tokens_details.cached_tokens): normalize_chunk bridges the
 *     spelling so the shared reader's cost.cache_hit accounting works,
 *     while extras keep the native fields.
 *
 * Header-only: the plugin .cpp and the provider-local tests include this
 * directly; nothing else links DeepSeek-specific code.
 */

#pragma once

#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"
#include "llm/chat_completions/dialect.hpp"

namespace llm::deepseek {

// ===== the provider dialect ===================================================

class DeepSeekDialect final : public llm::chat_completions::ChatCompletionsDialect {
public:
    model_io::ModelEndpoint default_endpoint() const override {
        model_io::ModelEndpoint endpoint;
        endpoint.base_url = "https://api.deepseek.com";
        endpoint.request_path = "/chat/completions";
        endpoint.auth.scheme = model_io::AuthScheme::Bearer;
        endpoint.user_agent = "simplex-cpp/deepseek";
        return endpoint;
    }

    std::string_view provider_name() const override { return "deepseek"; }

    bool replay_assistant_reasoning() const override {
        // Thinking mode + tools rejects (400) a request whose intermediate
        // assistant messages omit reasoning_content; a no-tools request
        // simply ignores the field, so replaying unconditionally is safe.
        // Verified against endpoint/example/deepseek_chat.cpp.
        return true;
    }

    void transform_request(nlohmann::json& body) const override {
        // An explicit native thinking object is the caller's exact intent:
        // untouched, even where a contradicting reasoning_effort rides
        // along (the server answers that self-contradiction itself).
        const bool native_thinking =
            body.contains("thinking") && body["thinking"].is_object();
        if (!native_thinking) {
            // Thinking defaults to enabled server-side; "none"/"minimal"
            // efforts are the closest spelling of "do not think".
            const bool disable = body.contains("reasoning_effort") &&
                                 body["reasoning_effort"].is_string() &&
                                 (body["reasoning_effort"] == "none" ||
                                  body["reasoning_effort"] == "minimal");
            body["thinking"] = {{"type", disable ? "disabled" : "enabled"}};
            if (disable) body.erase("reasoning_effort");
        }
        // The documented vocabulary clamp: only "high" and "max" exist, with
        // low/medium folded to high and xhigh raised to max.
        if (const auto effort = body.find("reasoning_effort");
            effort != body.end() && effort->is_string()) {
            const std::string& value = effort->get_ref<const std::string&>();
            if (value == "low" || value == "medium") {
                *effort = "high";
            } else if (value == "xhigh") {
                *effort = "max";
            }
        }
        // n is undocumented on DeepSeek; the two penalties are deprecated
        // no-ops. Stripped so a stale config cannot silently do nothing.
        body.erase("n");
        body.erase("frequency_penalty");
        body.erase("presence_penalty");
    }

    nlohmann::json normalize_chunk(nlohmann::json chunk) const override {
        // Bridge the cache-hit spelling into the OpenAI one the shared
        // reader consumes; the native fields stay for extras.
        const auto usage = chunk.find("usage");
        if (usage == chunk.end() || !usage->is_object()) return chunk;
        const auto hit = usage->find("prompt_cache_hit_tokens");
        if (hit == usage->end() || !hit->is_number()) return chunk;
        auto& details = (*usage)["prompt_tokens_details"];
        if (!details.is_object()) details = nlohmann::json::object();
        if (!details.contains("cached_tokens")) {
            details["cached_tokens"] = *hit;
        }
        return chunk;
    }
};

inline llm::chat_completions::ChatCompletionsDialectPtr deepseek_dialect() {
    static const auto dialect = std::make_shared<const DeepSeekDialect>();
    return dialect;
}

} // namespace llm::deepseek
