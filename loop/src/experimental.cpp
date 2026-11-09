#include "loop/experimental.hpp"
#include <limits>
#include <stdexcept>

namespace loop {
namespace {
constexpr auto key = "simplex.experimental";
using Json = nlohmann::json;

Json policy(const ExperimentalControl& control) {
    const char* type = "total";
    switch (control.token_type) {
        case ExperimentalControl::TokenType::Prompt: type = "prompt"; break;
        case ExperimentalControl::TokenType::Generated: type = "generated"; break;
        case ExperimentalControl::TokenType::CacheHit: type = "cache_hit"; break;
        case ExperimentalControl::TokenType::Total: break;
    }
    return {{"step_limit", control.steps}, {"token_limit", control.tokens}, {"token_type", type}};
}

bool add(std::uint64_t& sum, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - sum) return false;
    sum += value;
    return true;
}
} // namespace

std::optional<Json> experimental_report(const model_io::AgentInputState& state) {
    if (!state.extras || !state.extras->contains(key)) return std::nullopt;
    return state.extras->at(key);
}

void reset_experimental(model_io::AgentInputState& state, const ExperimentalControl& control) {
    if (!state.extras) state.extras = Json::object();
    (*state.extras)[key] = {{"policy", policy(control)}, {"steps", 0u}, {"tokens", 0u},
        {"reason", ""}, {"error", ""}};
}

std::string experimental_boundary(const model_io::AgentInputState& state,
    const ExperimentalControl& control) {
    if (!control.enabled()) return {};
    const auto report = experimental_report(state);
    if (!report || report->at("policy") != policy(control)) {
        throw std::runtime_error("experimental budget requires a new user message after configuration changes");
    }
    const auto error = report->at("error").get<std::string>();
    if (!error.empty()) throw std::runtime_error(error);
    return report->at("reason").get<std::string>();
}

void account_experimental(model_io::AgentInputState& state, const ExperimentalControl& control,
    const model_io::MessageItem& response) {
    if (!control.enabled()) return;
    auto& report = state.extras->at(key);
    auto steps = report.at("steps").get<std::uint64_t>();
    auto tokens = report.at("tokens").get<std::uint64_t>();
    std::string error;
    if (!add(steps, 1)) error = "experimental step counter overflow";
    if (control.tokens) {
        if (!response.cost) {
            error = "experimental token usage unavailable";
        } else {
            const auto& cost = *response.cost;
            std::uint64_t value = 0;
            switch (control.token_type) {
                case ExperimentalControl::TokenType::Prompt: value = cost.prompt; break;
                case ExperimentalControl::TokenType::Generated: value = cost.generated; break;
                case ExperimentalControl::TokenType::Total:
                    value = cost.prompt;
                    if (!add(value, cost.generated)) error = "experimental token counter overflow";
                    break;
                case ExperimentalControl::TokenType::CacheHit:
                    value = cost.cache_hit;
                    // Adapters preserve raw usage. Missing cache counters must
                    // not masquerade as a measured zero in research results.
                    if (!response.extras || !response.extras->contains("usage")) {
                        error = "experimental cache-hit usage unavailable";
                    } else {
                        const auto& usage = response.extras->at("usage");
                        bool measured = false;
                        for (const char* path : {"/prompt_tokens_details/cached_tokens",
                                "/input_tokens_details/cached_tokens", "/prompt_cache_hit_tokens"}) {
                            const Json::json_pointer pointer(path);
                            if (usage.contains(pointer) && usage.at(pointer).is_number_integer()
                                && (usage.at(pointer).is_number_unsigned()
                                    || usage.at(pointer).get<std::int64_t>() >= 0)
                                && usage.at(pointer) == value) measured = true;
                        }
                        if (!measured) error = "experimental cache-hit usage unavailable";
                    }
                    break;
            }
            if (error.empty() && !add(tokens, value)) error = "experimental token counter overflow";
        }
    }
    report["steps"] = steps;
    report["tokens"] = tokens;
    report["error"] = error;
    if (control.steps && steps > control.steps) report["reason"] = "cumulative_step_control";
    else if (control.tokens && tokens > control.tokens) report["reason"] = "cumulative_token_control";
}

void retain_experimental(model_io::AgentInputState& state, const model_io::AgentInputState& draft) {
    if (const auto report = experimental_report(draft)) {
        if (!state.extras) state.extras = Json::object();
        (*state.extras)[key] = *report;
    }
}
} // namespace loop
