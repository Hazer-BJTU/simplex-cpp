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
 *   - provider_info() attaches the account balance beside the catalogue
 *     (balance_path): GET https://api.deepseek.com/user/balance, same
 *     host/auth, its document verbatim;
 *   - thinking mode is ON by default and toggled by `thinking`:
 *     `{"type": "enabled" | "disabled"}` — an explicit native object in the
 *     config passes through verbatim, otherwise the dialect emits it:
 *     efforts "none"/"minimal" mean disabled, anything else enabled;
 *   - public reasoning efforts are none|low|high|max; remote model options
 *     expose low|high|max. The low-level dialect remains permissive: efforts
 *     not consumed by the thinking toggle pass through for server validation.
 *     medium/xhigh are compatibility values, not advertised options. minimal
 *     retains the local legacy disable alias above (unlike the current API's
 *     minimal-to-low mapping), unless native thinking is explicitly supplied;
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
#include "llm/compat/chat_completions/dialect.hpp"

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

    // No /v1 prefix anywhere on this host (see default_endpoint) — the
    // catalogue is https://api.deepseek.com/models.
    std::string models_path() const override { return "/models"; }

    // The account-balance companion provider_info() attaches after the
    // catalogue: https://api.deepseek.com/user/balance, same host and auth,
    // answered with {"is_available": bool, "balance_infos": [{currency,
    // total_balance, granted_balance, topped_up_balance}, ...]} — attached
    // verbatim as the "balance" member.
    std::string balance_path() const override { return "/user/balance"; }

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
        // Preserve remaining efforts, including compatibility and unknown
        // values, for server interpretation rather than clamping locally.
        // Public values are none|low|high|max; see the legacy minimal toggle
        // above. A native thinking object bypasses that toggle entirely.
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
