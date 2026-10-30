/**
 * @file dialect.hpp
 * @brief GLM's (Zhipu AI / BigModel) spelling of the OpenAI-compatible Chat
 *        Completions protocol: endpoint defaults, the always-on thinking
 *        policy, and the finish-reason vocabulary.
 *
 * GLM (https://docs.bigmodel.cn/cn/guide) speaks the chat-completions wire
 * with a few provider deviations, each carried by exactly one hook here:
 *
 *   - endpoint: POST https://open.bigmodel.cn/api/paas/v4/chat/completions
 *     with Bearer auth, and the catalogue at /api/paas/v4/models. The base
 *     URL stops at the version prefix so a GLM Coding Plan account only
 *     overrides base_url (https://open.bigmodel.cn/api/coding/paas/v4) and
 *     keeps both paths;
 *   - the GLM-5.3 family always thinks: `thinking.type: "disabled"` is
 *     rejected with HTTP 400 / code 1210 ("该模型始终思考…") and
 *     `reasoning_effort` accepts exactly low|high|max (verified 2026-10-30).
 *     Both are refused locally, naming the field, so a caller sees the
 *     mistake before a billed round trip;
 *   - medium/xhigh are translated (medium -> high, xhigh -> max) because the
 *     family's predecessors document those two as aliases of the values it
 *     does take. none/minimal/unknown are refused instead: the model cannot
 *     stop thinking, so silently landing on `low` would be a different
 *     request than the caller wrote;
 *   - frequency_penalty, presence_penalty and thinking_budget are accepted
 *     and ignored (200, verified 2026-10-30), so they are stripped rather
 *     than left looking like working controls;
 *   - tool_choice documents "auto" as its only value, and `n` is not part of
 *     the request at all (one choice per exchange);
 *   - finish_reason adds `sensitive` (moderation) and
 *     `model_context_window_exceeded` (context overflow) beside OpenAI's
 *     vocabulary. Both are mapped onto content_filter/length so the shared
 *     reader reports a ContentFiltered/LengthLimited end rather than a
 *     generic failure; `network_error` stays untouched and fails as it
 *     should;
 *   - usage already uses OpenAI's spelling, including
 *     prompt_tokens_details.cached_tokens — no normalisation is needed, and
 *     the shared reader's cost accounting works unchanged;
 *   - reasoning_content is replayed on intermediate assistant messages,
 *     which interleaved thinking with tools requires. Whether the service
 *     reuses it across turns is the caller's choice through
 *     thinking.clear_thinking (false opts into preserved thinking); the
 *     dialect leaves that member exactly as configured.
 *
 * Header-only: the plugin .cpp and the provider-local tests include this
 * directly; nothing else links GLM-specific code.
 */

#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"
#include "llm/compat/chat_completions/dialect.hpp"

namespace llm::glm {

// ===== model families ========================================================

/**
 * The GLM-5.3 family (glm-5.3, glm-5.3-flash, glm-5.3-flashx and any later
 * sibling): thinking cannot be disabled and the effort vocabulary is exactly
 * low|high|max.
 */
inline bool is_glm53(std::string_view model) {
    return model == "glm-5.3" || model.starts_with("glm-5.3-");
}

/**
 * Models documented as always thinking. GLM-5.3 requires thinking, whereas
 * GLM-4.7 enables it by default but allows disabling it per turn. Other
 * models keep the permissive path where
 * `disabled` is forwarded for the service to judge — the dialect does not
 * infer an unverified model's capabilities from its name.
 */
inline bool requires_thinking(std::string_view model) {
    return is_glm53(model);
}

// ===== request policy ========================================================

/**
 * Validate the provider controls and resolve the effective wire request.
 *
 * Used for construction, atomic option validation and the final wire body.
 * Top-level reasoning_effort takes precedence over the shared reasoning.effort
 * envelope, which is consumed either way (the service rejects unknown
 * top-level members). Diagnostics name fields and, at most, the configured
 * model — never the caller's value.
 *
 * @throws std::invalid_argument when a control cannot be expressed on this
 *         provider or on the configured model family.
 */
inline void prepare_generation(nlohmann::json& body) {
    if (!body.is_object()) {
        throw std::invalid_argument("GLM generation must be an object");
    }
    const auto model = body.find("model");
    if (model == body.end() || !model->is_string() ||
        model->get_ref<const std::string&>().empty()) {
        throw std::invalid_argument("GLM requires a nonempty model name");
    }
    const std::string& name = model->get_ref<const std::string&>();
    const bool always_thinks = requires_thinking(name);
    const bool glm53 = is_glm53(name);

    // An explicit typed null is how a patch clears a control (merge_patch has
    // no erase); drop it before validation so it does not read as a value.
    if (const auto effort = body.find("reasoning_effort");
        effort != body.end() && effort->is_null()) {
        body.erase(effort);
    }

    if (const auto envelope = body.find("reasoning");
        envelope != body.end()) {
        if (!envelope->is_object()) {
            throw std::invalid_argument("GLM reasoning must be an object");
        }
        const auto effort = envelope->find("effort");
        if (effort != envelope->end() && !effort->is_null() &&
            !body.contains("reasoning_effort")) {
            body["reasoning_effort"] = *effort;
        }
        body.erase("reasoning");
    }

    // thinking: the 5.3 family rejects `disabled` remotely; refuse locally.
    if (const auto thinking = body.find("thinking");
        thinking != body.end()) {
        if (!thinking->is_object()) {
            throw std::invalid_argument("GLM thinking must be an object");
        }
        if (const auto type = thinking->find("type");
            type != thinking->end()) {
            if (!type->is_string()) {
                throw std::invalid_argument("GLM thinking.type must be a string");
            }
            if (always_thinks && *type == "disabled") {
                throw std::invalid_argument(
                    "GLM " + name +
                    " always thinks; thinking.type must be enabled and the "
                    "depth is set through reasoning_effort (low, high or max)");
            }
        }
        if (const auto clear = thinking->find("clear_thinking");
            clear != thinking->end() && !clear->is_boolean()) {
            throw std::invalid_argument(
                "GLM thinking.clear_thinking must be Boolean");
        }
    }

    // reasoning_effort: the 5.3 family takes exactly low, high and max.
    if (const auto effort = body.find("reasoning_effort");
        effort != body.end()) {
        if (!effort->is_string() ||
            effort->get_ref<const std::string&>().empty()) {
            throw std::invalid_argument(
                "GLM reasoning_effort must be a nonempty string");
        }
        if (glm53) {
            // Documented aliases on the family's predecessors: low/medium
            // map to high and xhigh maps to max. Translating them keeps a
            // portable configuration working without inventing a depth.
            if (*effort == "medium") {
                *effort = "high";
            } else if (*effort == "xhigh") {
                *effort = "max";
            }
            if (*effort != "low" && *effort != "high" && *effort != "max") {
                throw std::invalid_argument(
                    "GLM " + name +
                    " reasoning_effort must be low, high or max; the model "
                    "cannot stop thinking");
            }
        }
    }

    // tool_choice: "auto" is the only documented value.
    if (const auto choice = body.find("tool_choice");
        choice != body.end()) {
        if (!choice->is_string() || *choice != "auto") {
            throw std::invalid_argument(
                "GLM accepts tool_choice \"auto\" only");
        }
    }

    // tool_stream: opt-in streaming of function-call arguments.
    if (const auto stream = body.find("tool_stream");
        stream != body.end() && !stream->is_boolean()) {
        throw std::invalid_argument("GLM tool_stream must be Boolean");
    }

    // Accepted by the service but ignored (verified 200 on 2026-10-30).
    for (const char* field :
         {"frequency_penalty", "presence_penalty", "thinking_budget"}) {
        body.erase(field);
    }
}

// ===== the provider dialect ==================================================

class GlmDialect final : public llm::chat_completions::ChatCompletionsDialect {
public:
    model_io::ModelEndpoint default_endpoint() const override {
        model_io::ModelEndpoint endpoint;
        // Stops at the version prefix: a GLM Coding Plan account re-points
        // base_url to .../api/coding/paas/v4 and keeps request_path and
        // models_path unchanged.
        endpoint.base_url = "https://open.bigmodel.cn/api/paas/v4";
        endpoint.request_path = "/chat/completions";
        endpoint.auth.scheme = model_io::AuthScheme::Bearer;
        endpoint.user_agent = "simplex-cpp/glm";
        return endpoint;
    }

    std::string_view provider_name() const override { return "glm"; }

    // Relative to base_url's path prefix, so the Coding Plan endpoint reaches
    // its own catalogue through the same hook.
    std::string models_path() const override { return "/models"; }

    bool replay_assistant_reasoning() const override {
        // Interleaved thinking with tools requires the intermediate
        // reasoning_content to travel next to the tool results. Replaying it
        // is accepted (verified 2026-10-30) whether or not the caller opts
        // into preserved thinking, so the dialect always carries it and lets
        // thinking.clear_thinking decide what the service does with it.
        return true;
    }

    void transform_request(nlohmann::json& body) const override {
        prepare_generation(body);
        // One choice per exchange; `n` is not part of the documented request
        // and the builder's fixed n = 1 must not leak onto this wire.
        body.erase("n");
    }

    nlohmann::json normalize_chunk(nlohmann::json chunk) const override {
        // Two finish_reason values lie outside OpenAI's vocabulary and would
        // otherwise reach the shared reader as a generic failure: `sensitive`
        // is a moderation verdict, `model_context_window_exceeded` a length
        // verdict. Map them onto the vocabulary the reader knows. The mapping
        // is one-way on purpose — the assembled MessageItem carries the
        // mapped value in extras, and the provider's own spelling is
        // documented rather than duplicated as a side channel.
        const auto choices = chunk.find("choices");
        if (choices == chunk.end() || !choices->is_array()) return chunk;
        for (auto& choice : *choices) {
            if (!choice.is_object()) continue;
            const auto reason = choice.find("finish_reason");
            if (reason == choice.end() || !reason->is_string()) continue;
            if (*reason == "sensitive") {
                *reason = "content_filter";
            } else if (*reason == "model_context_window_exceeded") {
                *reason = "length";
            }
        }
        return chunk;
    }
};

inline llm::chat_completions::ChatCompletionsDialectPtr glm_dialect() {
    static const auto dialect = std::make_shared<const GlmDialect>();
    return dialect;
}

} // namespace llm::glm
