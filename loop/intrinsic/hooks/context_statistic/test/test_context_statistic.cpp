#define BOOST_TEST_MODULE ContextStatisticHook

#include "loop/intrinsic/context_statistic/hook.hpp"
#include "loop/hook_registry.hpp"
#include "loop/loop.hpp"
#include "llm/models.hpp"
#include "tools/registry.hpp"

#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using model_io::AgentInputState;
using model_io::TokenCost;
using nlohmann::json;

loop::intrinsic::HookConfig config(std::uint64_t window) {
    return {
        .name = "context_statistic",
        .description = "Track context usage.",
        .config = {{"context_window_tokens", window}},
    };
}

void before_model(eventbus::EventBus& bus, AgentInputState& state) {
    loop::ModelContext context{
        state.system_prompt,
        state.tools,
        state.extras,
    };
    bus.publish(loop::BeforeModel{state, context});
    state.extras = std::move(context.extras);
}

void append_response(AgentInputState& state, std::optional<TokenCost> cost) {
    if (state.turns.empty()) {
        state.turns.emplace_back();
        state.turns.back().user_input.role = "user";
    }
    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.cost = cost;
    step.commit_sequence = ++state.loop->committed_response_sequence;
    state.turns.back().agent_loop_step.push_back(std::move(step));
    ++state.loop->completed_exchanges;
}

json status(const AgentInputState& state) {
    const auto value = model_io::external_status(state, "context_statistic");
    BOOST_REQUIRE(value.has_value());
    return *value;
}

/** Scripted complete responses for a real run() integration check. */
class FinalModel final : public llm::LLMModel {
public:
    explicit FinalModel(boost::asio::any_io_executor executor)
        : LLMModel(executor, {}) {
    }

    llm::LLMModelType model_type() const noexcept override {
        return llm::LLMModelType::Conversation;
    }

    std::vector<TokenCost> costs;
    std::size_t next = 0;

    boost::asio::awaitable<model_io::MessageItem> converse(
        AgentInputState) override {
        model_io::MessageItem response;
        response.type = model_io::MessageItemType::ModelResponse;
        response.role = "assistant";
        response.cost = costs.at(next++);
        co_return response;
    }
};

class PruneHook final : public loop::LoopHookInterface {
public:
    std::string_view name() const noexcept override {
        return "pruner";
    }

protected:
    Subscriptions subscribe(eventbus::EventBus& bus) override {
        Subscriptions subscriptions;
        eventbus::EventBus::ScopedSubscription owned{
            bus.subscribe<loop::EditOnRunFinished>([](const auto& event) {
                if (!event.state.turns.empty()) {
                    event.state.turns.back().agent_loop_step.clear();
                }
            })};
        subscriptions.push_back(std::move(owned));
        return subscriptions;
    }
};

class ProjectionFailureModel final : public llm::LLMModel {
public:
    explicit ProjectionFailureModel(boost::asio::any_io_executor executor)
        : LLMModel(executor, {}) {
    }

    llm::LLMModelType model_type() const noexcept override {
        return llm::LLMModelType::Conversation;
    }

    bool fail_projection = true;
    int exchanges = 0;

    boost::asio::awaitable<model_io::MessageItem> converse(
        AgentInputState) override {
        model_io::MessageItem response;
        response.type = model_io::MessageItemType::ModelResponse;
        response.role = "assistant";
        if (exchanges++ == 0) {
            response.cost = TokenCost{.prompt = 10, .generated = 2};
            model_io::InvokeQuery call;
            call.id = "call-1";
            call.name = "missing_tool";
            response.invokes = std::vector<model_io::InvokeQuery>{call};
        } else {
            response.cost = TokenCost{.prompt = 20, .generated = 3};
        }
        co_return response;
    }

    void integrate(AgentInputState& state,
                   const model_io::MessageItem& item) override {
        LLMModel::integrate(state, item);
        if (fail_projection &&
            item.type == model_io::MessageItemType::InvokeReturn) {
            throw std::runtime_error("projection failed");
        }
    }
};

loop::RunResult run_once(boost::asio::io_context& io, llm::LLMModel& model,
                         tools::ToolRegistry& tools, eventbus::EventBus& bus,
                         AgentInputState& state) {
    model_io::MessageItem input;
    input.role = "user";
    auto future = boost::asio::co_spawn(
        io,
        loop::run(model, tools, bus, io.get_executor(), state, true,
                  std::move(input)),
        boost::asio::use_future);
    io.restart();
    io.run();
    return future.get();
}

loop::RunResult resume_once(boost::asio::io_context& io, llm::LLMModel& model,
                            tools::ToolRegistry& tools, eventbus::EventBus& bus,
                            AgentInputState& state) {
    auto future = boost::asio::co_spawn(
        io,
        loop::run(model, tools, bus, io.get_executor(), state, false, {}),
        boost::asio::use_future);
    io.restart();
    io.run();
    return future.get();
}

} // namespace

BOOST_AUTO_TEST_CASE(step_and_final_response_update_flat_persistent_metrics) {
    eventbus::EventBus bus;
    loop::LoopHookRegistry registry(bus);
    registry.add(std::make_shared<loop::intrinsic::ContextStatisticHook>(config(128)));

    AgentInputState state;
    state.loop = model_io::LoopProgress{};
    state.extras = json{{"temperature", 0.5}};
    model_io::sync_external_status(state, "other", json{{"value", 7}});
    state.system_prompt.add_section(
        "system", "System", "A short prompt.",
        model_io::SectionStability::Immutable);

    before_model(bus, state);
    append_response(state, TokenCost{.prompt = 100, .generated = 20,
                                     .cache_hit = 40});
    bus.publish(loop::EditOnStepFinished{state});
    json first = status(state);
    for (auto it = first.begin(); it != first.end(); ++it) {
        BOOST_TEST(!it.value().is_structured());
    }
    BOOST_TEST(first.at("context_window_tokens") == 128);
    BOOST_TEST(first.at("last_exchange_prompt_tokens") == 100);
    BOOST_TEST(first.at("last_exchange_generated_tokens") == 20);
    BOOST_TEST(first.at("last_exchange_cache_hit_tokens") == 40);
    BOOST_TEST(first.at("last_exchange_total_tokens") == 120);
    BOOST_TEST(first.at("last_window_remaining_ratio") == 0.0625);
    BOOST_TEST(first.at("average_cache_hit_probability") == 0.4);
    const auto prompt_bytes = state.system_prompt.render().markdown.size();
    BOOST_TEST(first.at("estimated_zero_request_tokens")
               == prompt_bytes / 4 + (prompt_bytes % 4 != 0));
    BOOST_TEST(state.extras->at("temperature") == 0.5);
    BOOST_TEST(model_io::external_status(state, "other")->at("value") == 7);

    before_model(bus, state);
    append_response(state, TokenCost{.prompt = 200, .generated = 10,
                                     .cache_hit = 100});
    bus.publish(loop::EditOnStepFinished{state});
    json second = status(state);
    BOOST_TEST(second.at("max_exchange_prompt_tokens") == 200);
    BOOST_TEST(second.at("max_exchange_generated_tokens") == 20);
    BOOST_TEST(second.at("max_exchange_cache_hit_tokens") == 100);
    BOOST_TEST(second.at("max_exchange_total_tokens") == 210);
    BOOST_TEST(second.at("cumulative_exchange_prompt_tokens") == 300);
    BOOST_TEST(second.at("cumulative_exchange_generated_tokens") == 30);
    BOOST_TEST(second.at("cumulative_exchange_cache_hit_tokens") == 140);
    BOOST_TEST(second.at("cumulative_exchange_total_tokens") == 330);
    BOOST_TEST(second.at("average_cache_hit_probability").get<double>()
               == 140.0 / 300.0);
    BOOST_TEST(second.at("last_window_remaining_ratio") == -82.0 / 128.0);
    BOOST_TEST(second.at("accounted_exchanges_in_run") == 2);
    BOOST_TEST(second.at("accounted_commit_sequence") == 2);

    // The final hook sees the same tool-bearing last step but must not count it
    // twice. A new run and JSON round-trip then retain the lifetime totals even
    // after an old step has been pruned.
    const loop::RunResult finished{.status = loop::RunStatus::ExchangeLimit};
    bus.publish(loop::EditOnRunFinished{state, finished});
    BOOST_TEST(status(state).at("cumulative_exchange_total_tokens") == 330);

    state.turns.back().agent_loop_step.erase(
        state.turns.back().agent_loop_step.begin());
    state = json(state).get<AgentInputState>();
    state.loop->completed_exchanges = 0;
    before_model(bus, state);
    append_response(state, TokenCost{.prompt = 50, .generated = 5,
                                     .cache_hit = 0});
    const loop::RunResult completed{.status = loop::RunStatus::Completed};
    bus.publish(loop::EditOnRunFinished{state, completed});
    json third = status(state);
    BOOST_TEST(third.at("cumulative_exchange_prompt_tokens") == 350);
    BOOST_TEST(third.at("cumulative_exchange_generated_tokens") == 35);
    BOOST_TEST(third.at("cumulative_exchange_cache_hit_tokens") == 140);
    BOOST_TEST(third.at("cumulative_exchange_total_tokens") == 385);
    BOOST_TEST(third.at("average_cache_hit_probability") == 0.4);
    BOOST_TEST(third.at("sampled_exchange_count") == 3);

    // A newly constructed instance may use an edited YAML window while the
    // persisted usage totals stay intact. The last ratio uses the new window.
    BOOST_TEST(registry.set(
        std::make_shared<loop::intrinsic::ContextStatisticHook>(config(400))));
    state.loop->completed_exchanges = 0;
    before_model(bus, state);
    const json reconfigured = status(state);
    BOOST_TEST(reconfigured.at("context_window_tokens") == 400);
    BOOST_TEST(reconfigured.at("last_window_remaining_ratio") == 345.0 / 400.0);
    BOOST_TEST(reconfigured.at("cumulative_exchange_total_tokens") == 385);
}

BOOST_AUTO_TEST_CASE(missing_usage_and_zero_exchange_do_not_invent_costs) {
    eventbus::EventBus bus;
    loop::LoopHookRegistry registry(bus);
    registry.add(std::make_shared<loop::intrinsic::ContextStatisticHook>(config(100)));

    AgentInputState state;
    state.loop = model_io::LoopProgress{};
    before_model(bus, state);
    append_response(state, std::nullopt);
    const loop::RunResult finished{.status = loop::RunStatus::Completed};
    bus.publish(loop::EditOnRunFinished{state, finished});

    const json missing = status(state);
    BOOST_TEST(missing.at("last_exchange_total_tokens").is_null());
    BOOST_TEST(missing.at("last_window_remaining_ratio").is_null());
    BOOST_TEST(missing.at("average_cache_hit_probability").is_null());
    BOOST_TEST(missing.at("cumulative_exchange_total_tokens") == 0);
    BOOST_TEST(missing.at("sampled_exchange_count") == 0);

    state.loop->completed_exchanges = 0;
    bus.publish(loop::EditOnRunFinished{state, finished});
    BOOST_TEST(status(state).at("cumulative_exchange_total_tokens") == 0);
    BOOST_TEST(status(state).at("accounted_exchanges_in_run") == 0);
}

BOOST_AUTO_TEST_CASE(config_is_strict_and_yaml_is_loadable) {
    BOOST_CHECK_THROW(loop::intrinsic::ContextStatisticHook(config(0)),
                      std::invalid_argument);
    auto wrong_name = config(100);
    wrong_name.name = "other_hook";
    BOOST_CHECK_THROW(loop::intrinsic::ContextStatisticHook(std::move(wrong_name)),
                      std::invalid_argument);
    BOOST_CHECK_NO_THROW(loop::intrinsic::ContextStatisticHook(config(100)));
    for (const auto& invalid : {
             json("100"), json(-1), json(1.5), json(true)}) {
        auto noninteger = config(100);
        noninteger.config["context_window_tokens"] = invalid;
        BOOST_CHECK_THROW(loop::intrinsic::ContextStatisticHook(
                              std::move(noninteger)), std::invalid_argument);
    }
    auto malformed = config(100);
    malformed.config["unknown"] = 1;
    BOOST_CHECK_THROW(loop::intrinsic::ContextStatisticHook(std::move(malformed)),
                      std::invalid_argument);

    auto hook = loop::intrinsic::ContextStatisticHook::from_config();
    BOOST_TEST(hook->name() == "context_statistic");
    BOOST_TEST(hook->config().config.at("context_window_tokens") == 128000);
}

BOOST_AUTO_TEST_CASE(real_loop_accounts_final_response_once_per_run) {
    boost::asio::io_context io;
    FinalModel model(io.get_executor());
    model.costs = {
        TokenCost{.prompt = 10, .generated = 2, .cache_hit = 4},
        TokenCost{.prompt = 20, .generated = 3, .cache_hit = 6},
    };
    tools::ToolRegistry tools;
    eventbus::EventBus bus;
    loop::LoopHookRegistry hooks(bus);
    hooks.add(std::make_shared<loop::intrinsic::ContextStatisticHook>(config(100)));
    AgentInputState state;

    for (int run_index = 0; run_index < 2; ++run_index) {
        model_io::MessageItem input;
        input.role = "user";
        auto task = loop::run(model, tools, bus, io.get_executor(),
                              state, true, std::move(input));
        auto future = boost::asio::co_spawn(
            io, std::move(task), boost::asio::use_future);
        io.restart();
        io.run();
        BOOST_CHECK(future.get().status == loop::RunStatus::Completed);
    }

    const json recorded = status(state);
    BOOST_TEST(recorded.at("cumulative_exchange_prompt_tokens") == 30);
    BOOST_TEST(recorded.at("cumulative_exchange_generated_tokens") == 5);
    BOOST_TEST(recorded.at("cumulative_exchange_cache_hit_tokens") == 10);
    BOOST_TEST(recorded.at("cumulative_exchange_total_tokens") == 35);
    BOOST_TEST(recorded.at("sampled_exchange_count") == 2);
    BOOST_TEST(recorded.at("accounted_exchanges_in_run") == 1);
    BOOST_TEST(recorded.at("accounted_commit_sequence") == 2);
}

BOOST_AUTO_TEST_CASE(finish_edit_rollback_is_reconciled_across_runs) {
    for (const bool round_trip : {false, true}) {
        boost::asio::io_context io;
        FinalModel model(io.get_executor());
        model.costs = {
            TokenCost{.prompt = 10, .generated = 2},
            TokenCost{.prompt = 20, .generated = 3},
        };
        tools::ToolRegistry tools;
        eventbus::EventBus bus;
        loop::LoopHookRegistry hooks(bus);
        hooks.add(std::make_shared<loop::intrinsic::ContextStatisticHook>(config(100)));
        bool fail_once = true;
        eventbus::EventBus::ScopedSubscription failing{
            bus.subscribe<loop::EditOnRunFinished>([&](const auto&) {
                if (fail_once) {
                    fail_once = false;
                    throw std::runtime_error("later finish edit failed");
                }
            })};
        AgentInputState state;

        BOOST_CHECK(run_once(io, model, tools, bus, state).status
                   == loop::RunStatus::Failed);
        BOOST_TEST(state.loop->committed_response_sequence == 1);
        BOOST_TEST(status(state).at("cumulative_exchange_total_tokens") == 0);
        if (round_trip) {
            state = json(state).get<AgentInputState>();
        }
        BOOST_CHECK(run_once(io, model, tools, bus, state).status
                   == loop::RunStatus::Completed);
        BOOST_TEST(status(state).at("cumulative_exchange_total_tokens") == 35);
        BOOST_TEST(status(state).at("sampled_exchange_count") == 2);
        BOOST_TEST(status(state).at("accounted_commit_sequence") == 2);
    }
}

BOOST_AUTO_TEST_CASE(invalid_usage_and_overflow_do_not_partially_update_status) {
    for (const bool overflow : {false, true}) {
        eventbus::EventBus bus;
        loop::LoopHookRegistry hooks(bus);
        hooks.add(std::make_shared<loop::intrinsic::ContextStatisticHook>(config(100)));
        AgentInputState state;
        state.loop = model_io::LoopProgress{};
        before_model(bus, state);
        if (overflow) {
            json previous = status(state);
            previous["cumulative_exchange_prompt_tokens"] =
                std::numeric_limits<std::uint64_t>::max();
            previous["cumulative_exchange_total_tokens"] =
                std::numeric_limits<std::uint64_t>::max();
            model_io::sync_external_status(state, "context_statistic", previous);
        }
        const json unchanged = status(state);
        append_response(state, overflow
            ? TokenCost{.prompt = 1}
            : TokenCost{.prompt = 1, .cache_hit = 2});
        BOOST_CHECK_THROW(bus.publish(loop::EditOnRunFinished{
                              state, loop::RunResult{}}), std::exception);
        BOOST_TEST(status(state) == unchanged);
    }
}

BOOST_AUTO_TEST_CASE(corrupt_persisted_cache_totals_are_rejected) {
    eventbus::EventBus bus;
    loop::LoopHookRegistry hooks(bus);
    hooks.add(std::make_shared<loop::intrinsic::ContextStatisticHook>(config(100)));
    AgentInputState state;
    state.loop = model_io::LoopProgress{};
    before_model(bus, state);
    json corrupt = status(state);
    corrupt["cumulative_exchange_cache_hit_tokens"] = 1;
    model_io::sync_external_status(state, "context_statistic", corrupt);
    BOOST_CHECK_THROW(bus.publish(loop::EditOnRunFinished{
                          state, loop::RunResult{}}), std::logic_error);
    BOOST_TEST(status(state) == corrupt);
}

BOOST_AUTO_TEST_CASE(pruned_unaccounted_response_reports_a_gap) {
    eventbus::EventBus bus;
    loop::LoopHookRegistry hooks(bus);
    hooks.add(std::make_shared<loop::intrinsic::ContextStatisticHook>(config(100)));
    AgentInputState state;
    state.loop = model_io::LoopProgress{};
    before_model(bus, state);
    append_response(state, TokenCost{.prompt = 12});
    state.turns.back().agent_loop_step.clear();
    BOOST_CHECK_THROW(before_model(bus, state), std::logic_error);
    BOOST_TEST(status(state).at("cumulative_exchange_total_tokens") == 0);
}

BOOST_AUTO_TEST_CASE(replacement_keeps_statistic_before_pruning) {
    boost::asio::io_context io;
    FinalModel model(io.get_executor());
    model.costs = {
        TokenCost{.prompt = 10, .generated = 2},
        TokenCost{.prompt = 20, .generated = 3},
    };
    tools::ToolRegistry tools;
    eventbus::EventBus bus;
    loop::LoopHookRegistry hooks(bus);
    hooks.add(std::make_shared<loop::intrinsic::ContextStatisticHook>(config(100)));
    auto pruner = std::make_shared<PruneHook>();
    hooks.add(pruner);
    AgentInputState state;

    BOOST_CHECK(run_once(io, model, tools, bus, state).status
               == loop::RunStatus::Completed);
    BOOST_TEST(state.turns.back().agent_loop_step.empty());

    // set() appends callbacks, so remove dependent pruning before replacing.
    BOOST_TEST(hooks.remove(pruner->name()));
    BOOST_TEST(hooks.set(
        std::make_shared<loop::intrinsic::ContextStatisticHook>(config(200))));
    hooks.add(pruner);
    BOOST_CHECK(run_once(io, model, tools, bus, state).status
               == loop::RunStatus::Completed);
    BOOST_TEST(status(state).at("cumulative_exchange_total_tokens") == 35);
    BOOST_TEST(status(state).at("context_window_tokens") == 200);
    BOOST_TEST(state.turns.back().agent_loop_step.empty());
}

BOOST_AUTO_TEST_CASE(projection_failure_usage_is_not_double_counted_on_recovery) {
    boost::asio::io_context io;
    ProjectionFailureModel model(io.get_executor());
    tools::ToolRegistry tools;
    eventbus::EventBus bus;
    loop::LoopHookRegistry hooks(bus);
    hooks.add(std::make_shared<loop::intrinsic::ContextStatisticHook>(config(100)));
    AgentInputState state;

    BOOST_CHECK(run_once(io, model, tools, bus, state).status
               == loop::RunStatus::Failed);
    BOOST_CHECK(state.loop->phase == model_io::LoopPhase::Projection);
    BOOST_TEST(status(state).at("cumulative_exchange_total_tokens") == 12);
    state = json(state).get<AgentInputState>();
    model.fail_projection = false;
    BOOST_CHECK(resume_once(io, model, tools, bus, state).status
               == loop::RunStatus::Completed);
    BOOST_TEST(status(state).at("cumulative_exchange_total_tokens") == 35);
    BOOST_TEST(status(state).at("sampled_exchange_count") == 2);
}
