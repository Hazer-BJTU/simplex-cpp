#include "loop/intrinsic/context_statistic/hook.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace loop::intrinsic {
namespace {

using model_io::AgentInputState;
using model_io::TokenCost;
using nlohmann::json;

constexpr std::string_view kSource = ContextStatisticHook::kName;

std::uint64_t add_checked(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error("context statistic token count overflow");
    }
    return left + right;
}

std::uint64_t count(const json& status, std::string_view key) {
    const json& value = status.at(key);
    if (!value.is_number_integer()
        || (value.is_number_integer() && !value.is_number_unsigned()
            && value.get<std::int64_t>() < 0)) {
        throw std::logic_error("context statistic has invalid field: "
                               + std::string(key));
    }
    return value.get<std::uint64_t>();
}

double remaining_ratio(std::uint64_t window, std::uint64_t total) {
    return static_cast<double>(
        (static_cast<long double>(window) - static_cast<long double>(total))
        / static_cast<long double>(window));
}

void refresh_last_ratio(json& status, std::uint64_t window) {
    const auto last = status.find("last_exchange_total_tokens");
    if (last == status.end()) {
        throw std::logic_error("context statistic last exchange field is missing");
    }
    status["last_window_remaining_ratio"] = last->is_null()
        ? json(nullptr)
        : json(remaining_ratio(window, count(status, "last_exchange_total_tokens")));
}

void clear_last(json& status) {
    status["last_exchange_prompt_tokens"] = nullptr;
    status["last_exchange_generated_tokens"] = nullptr;
    status["last_exchange_cache_hit_tokens"] = nullptr;
    status["last_exchange_total_tokens"] = nullptr;
    status["last_window_remaining_ratio"] = nullptr;
}

json empty_status(std::uint64_t window) {
    json status = {
        {"context_window_tokens", window},
        {"estimated_zero_request_tokens", 0},
        {"max_exchange_prompt_tokens", 0},
        {"max_exchange_generated_tokens", 0},
        {"max_exchange_cache_hit_tokens", 0},
        {"max_exchange_total_tokens", 0},
        {"cumulative_exchange_prompt_tokens", 0},
        {"cumulative_exchange_generated_tokens", 0},
        {"cumulative_exchange_cache_hit_tokens", 0},
        {"cumulative_exchange_total_tokens", 0},
        {"average_cache_hit_probability", nullptr},
        {"sampled_exchange_count", 0},
        {"accounted_exchanges_in_run", 0},
        {"accounted_commit_sequence", 0},
    };
    clear_last(status);
    return status;
}

void account(json& status, const std::optional<TokenCost>& cost,
             std::uint64_t window) {
    clear_last(status);
    if (!cost) {
        return;
    }
    if (cost->cache_hit > cost->prompt) {
        throw std::logic_error("cache-hit tokens exceed prompt tokens");
    }

    const std::uint64_t total = add_checked(cost->prompt, cost->generated);
    status["last_exchange_prompt_tokens"] = cost->prompt;
    status["last_exchange_generated_tokens"] = cost->generated;
    status["last_exchange_cache_hit_tokens"] = cost->cache_hit;
    status["last_exchange_total_tokens"] = total;
    status["last_window_remaining_ratio"] = remaining_ratio(window, total);

    for (const auto& [field, value] : {
             std::pair{"prompt", cost->prompt},
             std::pair{"generated", cost->generated},
             std::pair{"cache_hit", cost->cache_hit},
             std::pair{"total", total},
         }) {
        const std::string maximum = "max_exchange_" + std::string(field) + "_tokens";
        const std::string cumulative =
            "cumulative_exchange_" + std::string(field) + "_tokens";
        status[maximum] = std::max(count(status, maximum), value);
        status[cumulative] = add_checked(count(status, cumulative), value);
    }
    status["sampled_exchange_count"] =
        add_checked(count(status, "sampled_exchange_count"), 1);

    const std::uint64_t prompt = count(status, "cumulative_exchange_prompt_tokens");
    const std::uint64_t hits = count(status, "cumulative_exchange_cache_hit_tokens");
    status["average_cache_hit_probability"] = prompt == 0
        ? json(nullptr)
        : json(static_cast<double>(hits) / static_cast<double>(prompt));
}

/** Initialize from retained history only when no prior statistics exist. */
json bootstrap(const AgentInputState& state, std::uint64_t window) {
    json status = empty_status(window);
    std::uint64_t checkpoint = 0;
    for (const auto& turn : state.turns) {
        for (const auto& step : turn.agent_loop_step) {
            if (step.commit_sequence != 0) {
                if (step.commit_sequence != add_checked(checkpoint, 1)) {
                    throw std::logic_error("context statistic cannot bootstrap pruned response usage");
                }
                checkpoint = step.commit_sequence;
            }
            account(status, step.model_response.cost, window);
        }
    }
    const auto committed = state.loop
        ? state.loop->committed_response_sequence : 0;
    if (checkpoint != committed) {
        throw std::logic_error("context statistic cannot bootstrap missing response usage");
    }
    status["accounted_commit_sequence"] = checkpoint;
    return status;
}

/** Advance the persisted checkpoint in commit order, with totals in one draft. */
void reconcile(json& status, const AgentInputState& state, std::uint64_t window) {
    std::uint64_t checkpoint = count(status, "accounted_commit_sequence");
    const auto committed = state.loop
        ? state.loop->committed_response_sequence : 0;
    if (checkpoint > committed) {
        throw std::logic_error("context statistic checkpoint exceeds committed responses");
    }
    for (const auto& turn : state.turns) {
        for (const auto& step : turn.agent_loop_step) {
            const auto sequence = step.commit_sequence;
            if (sequence == 0 || sequence <= checkpoint) {
                continue;
            }
            if (sequence != add_checked(checkpoint, 1)) {
                throw std::logic_error("context statistic committed response accounting gap");
            }
            if (step.model_response.type != model_io::MessageItemType::ModelResponse) {
                throw std::logic_error("context statistic expected a model response");
            }
            account(status, step.model_response.cost, window);
            checkpoint = sequence;
        }
    }
    if (checkpoint != committed) {
        throw std::logic_error("context statistic response was pruned before accounting");
    }
    status["accounted_commit_sequence"] = checkpoint;
}

/** Prepare one candidate; callers refresh derived fields and publish it once. */
json read_statistics(const AgentInputState& state,
                     std::optional<json> existing,
                     std::uint64_t window) {
    if (!existing) {
        return bootstrap(state, window);
    }
    json candidate = std::move(*existing);
    const auto prompt = count(candidate, "cumulative_exchange_prompt_tokens");
    const auto generated = count(candidate, "cumulative_exchange_generated_tokens");
    const auto hits = count(candidate, "cumulative_exchange_cache_hit_tokens");
    const auto total = count(candidate, "cumulative_exchange_total_tokens");
    if (hits > prompt || add_checked(prompt, generated) != total) {
        throw std::logic_error("context statistic has inconsistent cumulative usage");
    }
    (void)count(candidate, "sampled_exchange_count");
    reconcile(candidate, state, window);
    return candidate;
}

std::uint64_t estimated_zero_request_tokens(
    const model_io::PromptTemplate& prompt,
    const std::vector<model_io::Invocable>& tools) {
    std::uint64_t bytes = prompt.render().markdown.size();
    if (!tools.empty()) {
        json definitions = json::array();
        for (const auto& tool : tools) {
            definitions.push_back({
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", tool.argument_schema},
            });
        }
        bytes = add_checked(bytes, definitions.dump().size());
    }
    // A deliberately simple UTF-8 byte heuristic. Provider wrappers and
    // tokenizer-specific behavior cannot be recovered from AgentInputState.
    return bytes / 4 + (bytes % 4 != 0);
}

void refresh_fixed_fields(json& status, const AgentInputState& state,
                          std::uint64_t window) {
    status["context_window_tokens"] = window;
    status["estimated_zero_request_tokens"] =
        estimated_zero_request_tokens(state.system_prompt, state.tools);
    refresh_last_ratio(status, window);
}

} // namespace

ContextStatisticHook::ContextStatisticHook(HookConfig config)
    : IntrinsicLoopHook(std::move(config)),
      context_window_tokens_(0) {
    if (this->config().name != kName) {
        throw std::invalid_argument(
            "ContextStatisticHook requires name 'context_statistic'");
    }
    const json& options = this->config().config;
    if (options.size() != 1 || !options.contains("context_window_tokens")) {
        throw std::invalid_argument(
            "context_statistic config requires only context_window_tokens");
    }
    const json& window = options.at("context_window_tokens");
    if (!window.is_number_integer()
        || (window.is_number_integer() && !window.is_number_unsigned()
            && window.get<std::int64_t>() <= 0)) {
        throw std::invalid_argument("context_window_tokens must be a positive integer");
    }
    context_window_tokens_ = window.get<std::uint64_t>();
    if (context_window_tokens_ == 0) {
        throw std::invalid_argument("context_window_tokens must be positive");
    }
}

std::shared_ptr<ContextStatisticHook> ContextStatisticHook::from_config() {
    return std::make_shared<ContextStatisticHook>(
        load_hook_config(hook_config_file(kName), kName));
}

LoopHookInterface::Subscriptions ContextStatisticHook::subscribe(
    eventbus::EventBus& bus) {
    Subscriptions subscriptions;
    eventbus::EventBus::ScopedSubscription before{
        bus.subscribe<BeforeModel>(
            [this](const BeforeModel& event) { before_model(event); })};
    subscriptions.push_back(std::move(before));
    eventbus::EventBus::ScopedSubscription step{
        bus.subscribe<EditOnStepFinished>(
            [this](const EditOnStepFinished& event) { on_step_finished(event); })};
    subscriptions.push_back(std::move(step));
    eventbus::EventBus::ScopedSubscription finish{
        bus.subscribe<EditOnRunFinished>(
            [this](const EditOnRunFinished& event) { on_run_finished(event); })};
    subscriptions.push_back(std::move(finish));
    return subscriptions;
}

void ContextStatisticHook::before_model(const BeforeModel& event) const {
    AgentInputState candidate;
    candidate.extras = std::move(event.context.extras);
    json status = read_statistics(
        event.state,
        model_io::external_status(candidate, kSource),
        context_window_tokens_);
    if (event.state.loop && event.state.loop->completed_exchanges == 0) {
        status["accounted_exchanges_in_run"] = 0;
    }
    status["context_window_tokens"] = context_window_tokens_;
    status["estimated_zero_request_tokens"] = estimated_zero_request_tokens(
        event.context.system_prompt, event.context.tools);
    refresh_last_ratio(status, context_window_tokens_);
    model_io::sync_external_status(candidate, kSource, std::move(status));
    event.context.extras = std::move(candidate.extras);
}

void ContextStatisticHook::on_step_finished(
    const EditOnStepFinished& event) const {
    update(event.state);
}

void ContextStatisticHook::on_run_finished(
    const EditOnRunFinished& event) const {
    update(event.state);
}

void ContextStatisticHook::update(AgentInputState& state) const {
    json status = read_statistics(
        state,
        model_io::external_status(state, kSource),
        context_window_tokens_);
    status["accounted_exchanges_in_run"] = state.loop
        ? state.loop->completed_exchanges : 0;
    refresh_fixed_fields(status, state, context_window_tokens_);
    model_io::sync_external_status(state, kSource, std::move(status));
}

} // namespace loop::intrinsic
