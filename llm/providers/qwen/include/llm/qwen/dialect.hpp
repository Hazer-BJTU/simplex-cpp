#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "llm/compat/chat_completions/dialect.hpp"

namespace llm::qwen {

/** Whether the model belongs to the documented Qwen 3.8 text/vision families. */
inline bool is_qwen38(const std::string& model) {
    return model == "qwen3.8-flash" || model == "qwen3.8-max"
        || model.starts_with("qwen3.8-flash-")
        || model.starts_with("qwen3.8-max-");
}

/**
 * Validate provider controls and resolve their effective defaults on a copy.
 *
 * Used for construction, atomic option validation and the final wire request.
 * Top-level reasoning_effort takes precedence over the shared reasoning.effort
 * envelope, as in the neutral interpreter. Omitted controls on other model
 * families remain omitted: their capabilities/defaults are not inferred from
 * the two advertised models. Diagnostic messages name fields, never values.
 */
inline void prepare_generation(nlohmann::json& body) {
    if (!body.is_object() || !body.contains("model")
        || !body["model"].is_string() || body["model"].get_ref<const std::string&>().empty()) {
        throw std::invalid_argument("Qwen requires a nonempty model name");
    }
    if (body.contains("extra_body") || body.contains("thinking")) {
        throw std::invalid_argument(
            "Qwen controls belong at the top level; use enable_thinking, not extra_body or thinking");
    }
    if (const auto reasoning = body.find("reasoning"); reasoning != body.end()) {
        if (!reasoning->is_object()) {
            throw std::invalid_argument("Qwen reasoning must be an object");
        }
        if (!body.contains("reasoning_effort") && reasoning->contains("effort")) {
            body["reasoning_effort"] = reasoning->at("effort");
        }
        body.erase("reasoning");
    }
    for (const char* field : {"enable_thinking", "preserve_thinking", "tool_stream"}) {
        if (body.contains(field) && !body[field].is_boolean()) {
            throw std::invalid_argument(std::string("Qwen ") + field + " must be Boolean");
        }
    }
    const auto& model = body["model"].get_ref<const std::string&>();
    const bool known = is_qwen38(model);
    if (body.contains("reasoning_effort") && body.contains("thinking_budget")) {
        throw std::invalid_argument("Qwen reasoning_effort and thinking_budget are mutually exclusive");
    }
    if (body.contains("reasoning_effort")) {
        auto& effort = body["reasoning_effort"];
        if (!effort.is_string() || effort.get_ref<const std::string&>().empty()) {
            throw std::invalid_argument("Qwen reasoning_effort must be a nonempty string");
        }
        if (known) {
            if (effort == "high" || effort == "max") {
                effort = "xhigh";
            }
            if (effort != "low" && effort != "medium" && effort != "xhigh") {
                throw std::invalid_argument("Qwen 3.8 reasoning_effort must be low, medium or xhigh");
            }
        }
    }
    if (body.contains("thinking_budget")) {
        const auto& budget = body["thinking_budget"];
        if (!budget.is_number_integer() || budget < 0
            || (known && budget > 262144)) {
            throw std::invalid_argument("Qwen thinking_budget must be a nonnegative integer (at most 262144 for Qwen 3.8)");
        }
    }
    if (known) {
        if (!body.contains("enable_thinking")) {
            body["enable_thinking"] = true;
        }
        if (!body.contains("preserve_thinking")) {
            body["preserve_thinking"] = model == "qwen3.8-max"
                || model.starts_with("qwen3.8-max-");
        }
        if (!body.contains("reasoning_effort") && !body.contains("thinking_budget")) {
            // Intentional plugin default: explicit xhigh permits 262144 tokens,
            // above the service's 131072 budget when both controls are absent.
            body["reasoning_effort"] = "xhigh";
        }
    }
    // The shared reader only assembles text responses. Do not silently request
    // an audio response that it cannot represent.
    if (body.contains("audio")
        || (body.contains("modalities")
            && body["modalities"] != nlohmann::json::array({"text"}))) {
        throw std::invalid_argument("Qwen provider supports text output only");
    }
    if (body.contains("tool_choice") && body["tool_choice"] == "required") {
        throw std::invalid_argument("Qwen does not support tool_choice required");
    }
}

/** Qianwen AI's Chat Completions dialect; no private transport or retry state. */
class QwenDialect final : public llm::chat_completions::ChatCompletionsDialect {
public:
    model_io::ModelEndpoint default_endpoint() const override {
        model_io::ModelEndpoint endpoint;
        endpoint.base_url = "https://maas.qianwenaiapi.com";
        endpoint.request_path = "/compatible-mode/v1/chat/completions";
        endpoint.auth.scheme = model_io::AuthScheme::Bearer;
        endpoint.user_agent = "simplex-cpp/qwen";
        return endpoint;
    }

    std::string_view provider_name() const override { return "qwen"; }

    // Collect reasoning without loss; transform_request decides whether this
    // particular model/request opts into replay. The dialect remains immutable.
    bool replay_assistant_reasoning() const override { return true; }

    void transform_request(nlohmann::json& body) const override {
        prepare_generation(body);
        body.erase("n");
        if (!body.value("preserve_thinking", false)) {
            for (auto& message : body["messages"]) {
                message.erase("reasoning_content");
            }
        }
    }
};

inline llm::chat_completions::ChatCompletionsDialectPtr qwen_dialect() {
    static const auto dialect = std::make_shared<const QwenDialect>();
    return dialect;
}

} // namespace llm::qwen
