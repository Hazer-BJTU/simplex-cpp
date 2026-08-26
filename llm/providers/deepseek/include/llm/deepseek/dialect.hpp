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
 *   - `reasoning_effort` passes through verbatim. Live 2026-08 the server's
 *     own vocabulary is none|minimal|low|medium|high|xhigh|max (an
 *     out-of-vocabulary value is rejected with that enumeration in the 400
 *     body), so an earlier client-side clamp to high/max — based on docs
 *     that have since been restructured — only distorted the caller's
 *     intent and is gone;
 *   - intermediate assistant messages replay their reasoning_content
 *     (replay_assistant_reasoning): the endpoint prototype era documented a
 *     hard 400 when thinking+tools omitted it; live 2026-08 the omission is
 *     accepted, and replay is kept because it is the canonical multi-turn
 *     shape the docs' samples use and it keeps the replayed prefix
 *     byte-stable for DeepSeek's automatic context cache;
 *   - `n` is rejected server-side ("Invalid n value (currently only n = 1 is
 *     supported)", live 2026-08) and frequency_penalty/presence_penalty are
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
        // The endpoint prototype (endpoint/example/deepseek_chat.cpp)
        // recorded a hard rule: thinking+tools answers 400 when an
        // intermediate assistant message omits reasoning_content. A 2026-08
        // live probe shows the omission is accepted now, so this is no
        // longer load-bearing correctness — replay stays because it is the
        // canonical multi-turn shape the docs' samples use, and because the
        // replayed prefix stays byte-stable, which DeepSeek's automatic
        // context cache rewards (observed: cache_hit grows turn over turn).
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
        // reasoning_effort passes through verbatim: live 2026-08 the server
        // accepts none|minimal|low|medium|high|xhigh|max and rejects anything
        // else with a 400 that enumerates the vocabulary, so an invalid value
        // fails loudly at the server instead of being silently reshaped here.
        // (An earlier clamp low/medium→high, xhigh→max mirrored since-
        // restructured docs and distorted the caller's intent.)
        // n is rejected server-side ("currently only n = 1 is supported");
        // the two penalties are deprecated no-ops. All three stripped so a
        // stale config cannot fail the request or silently do nothing.
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
