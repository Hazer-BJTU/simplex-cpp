#define BOOST_TEST_MODULE AgentLoop

#include <boost/test/unit_test.hpp>
#include "loop/loop.hpp"
#include "loop/events.hpp"
#include "llm/models.hpp"
#include "tools/registry.hpp"
#include "eventbus/event_bus.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>
#include <functional>
#include <future>
#include <thread>
#include <boost/asio/steady_timer.hpp>

namespace asio = boost::asio;

namespace {

using State = model_io::AgentInputState;
using Item = model_io::MessageItem;
using Kind = model_io::MessageItemType;
/** Creates the minimal user input shared by the offline loop tests. */
Item input() {
    Item value;
    value.role = "user";
    return value;
}

/**
 * Scripted provider: requests tools on its first exchange, then returns a final
 * answer. Failure switches exercise model errors and partial draft integration.
 */
struct Model : llm::LLMModel {
    /// Binds the fake provider to the fixture executor; no network is used.
    explicit Model(asio::any_io_executor executor)
        : LLMModel(executor, {}) {
    }

    /// Advertises the conversation interface exercised by the loop.
    llm::LLMModelType model_type() const noexcept override {
        return llm::LLMModelType::Conversation;
    }

    int exchanges = 0;
    int call_count = 1;
    bool calls = true;
    bool fail_model = false;
    bool fail_projection = false;
    std::function<void(const State&)> inspect;

    /// Inspects the received state and returns the next scripted response.
    asio::awaitable<Item> converse(State state) override {
        if (inspect) {
            inspect(state);
        }
        if (fail_model) {
            throw std::runtime_error("model failure");
        }

        Item response;
        response.type = Kind::ModelResponse;
        response.role = "assistant";
        if (exchanges++ == 0 && calls) {
            model_io::InvokeQuery query;
            query.id = "call";
            query.name = "effect";
            response.invokes.emplace();
            for (int i = 0; i < call_count; ++i) {
                query.id = "call" + std::to_string(i);
                response.invokes->push_back(query);
            }
        }
        co_return response;
    }

    /// Can throw after mutating the candidate to verify atomic projection.
    void integrate(State& state, const Item& item) override {
        LLMModel::integrate(state, item);
        if (fail_projection && item.type == Kind::InvokeReturn) {
            throw std::runtime_error("projection failure after draft mutation");
        }
    }
};

/** Controlled tool with an observable effect and an explicit suspension point. */
struct Tool : tools::ToolInterface {
    model_io::Invocable details;
    int count = 0;
    bool parallel = false;
    std::function<void()> after_effect;

    /// Gives the registry a stable tool name for the scripted model calls.
    Tool() {
        details.name = "effect";
    }

    /// Returns immutable registration metadata for this fixture tool.
    const model_io::Invocable& get_details() const noexcept override {
        return details;
    }

    /// Selects serial or parallel dispatch without involving confirmation UI.
    void write_attributes(model_io::InvokeQuery& query) const override {
        query.security = model_io::InvokeSecurity::Trusted;
        query.type = parallel
            ? model_io::InvokeType::ParallWrite
            : model_io::InvokeType::SerialWrite;
    }

    /// Counts the effect before yielding; the callback then stops or fails work.
    asio::awaitable<model_io::Content> invoke(const model_io::InvokeQuery&) override {
        ++count;
        co_await asio::post(asio::use_awaitable);
        if (after_effect) {
            after_effect();
        }

        model_io::Content content;
        content.raw = "effect completed";
        co_return content;
    }
};

/** Registers the controlled tool through the real ToolSet/registry protocol. */
struct Set : tools::ToolSet {
    std::shared_ptr<Tool> tool = std::make_shared<Tool>();

    /// Supplies the fixture's toolset identity.
    std::string_view name() const noexcept override {
        return "test";
    }

    /// Advertises the single tool available to this fixture.
    std::vector<model_io::Invocable> get_tools() const override {
        return {tool->details};
    }

    /// Resolves calls already routed to this set by the registry.
    ToolHandle dispatch(const model_io::InvokeQuery&) const override {
        return tool;
    }
};

/** Owns all borrowed services and state until each test run has fully drained. */
struct Fixture {
    asio::io_context io;
    Model model{io.get_executor()};
    tools::ToolRegistry registry;
    eventbus::EventBus bus;
    State state;
    std::shared_ptr<Set> set = std::make_shared<Set>();

    /// Registers the fixture tool before any model exchange is attempted.
    Fixture() {
        registry.add(set);
    }

    /// Runs one coroutine to completion on the fixture's single-thread executor.
    loop::RunResult run(
        bool has_message = true,
        Item message = input(),
        std::size_t budget = 5,
        std::stop_token stop = {}) {
        auto task = loop::run(
            model,
            registry,
            bus,
            io.get_executor(),
            state,
            has_message,
            std::move(message),
            {budget},
            stop);
        auto future = asio::co_spawn(io, std::move(task), asio::use_future);
        io.restart();
        io.run();
        return future.get();
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(normal_cycle_and_event_order) {
    Fixture f;
    std::vector<std::string> events;
    auto a = f.bus.subscribe<loop::RunStarted>([&](const auto&) {
        events.push_back("start");
    });
    auto b = f.bus.subscribe<loop::InputCommitted>([&](const auto&) {
        events.push_back("input");
    });
    auto c = f.bus.subscribe<loop::BeforeModel>([&](const auto&) {
        events.push_back("model");
    });
    auto d = f.bus.subscribe<loop::ModelCommitted>([&](const auto&) {
        events.push_back("response");
    });
    auto e = f.bus.subscribe<loop::BeforeToolBatch>([&](const auto&) {
        events.push_back("tools");
    });
    auto g = f.bus.subscribe<loop::ToolResultsCommitted>([&](const auto&) {
        events.push_back("results");
    });
    auto h = f.bus.subscribe<loop::RunFinished>([&](const auto& event) {
        BOOST_CHECK(event.state.loop->status == model_io::LoopStatus::Completed);
        events.push_back("end");
    });
    auto result = f.run();
    BOOST_CHECK(result.status == loop::RunStatus::Completed);
    BOOST_CHECK_EQUAL(result.completed_exchanges, 2u);
    BOOST_CHECK_EQUAL(f.set->tool->count, 1);
    const std::vector<std::string> expected{
        "start", "input", "model", "response", "tools",
        "results", "model", "response", "end"
    };
    BOOST_CHECK(events == expected);
    BOOST_REQUIRE(f.state.turns[0].agent_loop_step[0].invoke_returns);
    BOOST_CHECK_EQUAL(
        f.state.turns[0].agent_loop_step[0].invoke_returns->at(0).content[0].raw,
        "effect completed");
}

BOOST_AUTO_TEST_CASE(writable_hooks_are_ordered_and_reach_model) {
    Fixture f;
    f.model.calls = false;
    auto a = f.bus.subscribe<loop::BeforeInput>([](const auto& e) {
        e.input.role = "edited";
    });
    auto b = f.bus.subscribe<loop::BeforeInput>([](const auto& e) {
        BOOST_CHECK_EQUAL(e.input.role, "edited");
    });
    auto c = f.bus.subscribe<loop::BeforeModel>([](const auto& e) {
        e.context.extras = nlohmann::json{{"custom", 42}};
    });
    f.model.inspect = [](const State& state) {
        BOOST_CHECK_EQUAL(state.turns[0].user_input.role, "edited");
        BOOST_CHECK_EQUAL(state.extras->at("custom").get<int>(), 42);
    };
    BOOST_CHECK(f.run().status == loop::RunStatus::Completed);
}

BOOST_AUTO_TEST_CASE(throwing_writable_hook_discards_draft) {
    Fixture f;
    auto sub = f.bus.subscribe<loop::BeforeModel>([](const auto& e) {
        e.context.extras = nlohmann::json{{"uncommitted", true}};
        throw std::runtime_error("hook failed");
    });
    BOOST_CHECK(f.run().status == loop::RunStatus::Failed);
    BOOST_CHECK(!f.state.extras);
    BOOST_CHECK_EQUAL(f.model.exchanges, 0);
    BOOST_CHECK_EQUAL(f.state.turns.size(), 1u);
}

BOOST_AUTO_TEST_CASE(projection_failure_roundtrips_and_recovers_without_reexecution) {
    Fixture f;
    f.model.fail_projection = true;
    BOOST_CHECK(f.run().status == loop::RunStatus::Failed);
    BOOST_CHECK(f.state.loop->phase == model_io::LoopPhase::Projection);
    BOOST_REQUIRE_EQUAL(f.state.loop->pending_results.size(), 1u);
    BOOST_CHECK(!f.state.turns[0].agent_loop_step[0].invoke_returns);
    f.state = nlohmann::json(f.state).get<State>();
    f.model.fail_projection = false;
    BOOST_CHECK(f.run(false).status == loop::RunStatus::Completed);
    BOOST_CHECK_EQUAL(f.set->tool->count, 1);
    BOOST_CHECK(f.state.loop->pending_results.empty());
    BOOST_CHECK_EQUAL(f.state.turns.size(), 1u);
}

BOOST_AUTO_TEST_CASE(stop_during_tool_drains_and_commits) {
    Fixture f;
    std::stop_source source;
    f.set->tool->after_effect = [&] {
        source.request_stop();
    };
    BOOST_CHECK(f.run(true, input(), 5, source.get_token()).status == loop::RunStatus::Cancelled);
    BOOST_CHECK_EQUAL(f.model.exchanges, 1);
    BOOST_CHECK_EQUAL(f.set->tool->count, 1);
    BOOST_CHECK(f.state.loop->phase == model_io::LoopPhase::Ready);
    BOOST_REQUIRE(f.state.turns[0].agent_loop_step[0].invoke_returns);
}

BOOST_AUTO_TEST_CASE(stop_before_batch_closes_calls_without_effects) {
    Fixture f;
    std::stop_source source;
    auto sub = f.bus.subscribe<loop::BeforeToolBatch>([&](const auto&) {
        source.request_stop();
    });
    BOOST_CHECK(f.run(true, input(), 5, source.get_token()).status == loop::RunStatus::Cancelled);
    BOOST_CHECK_EQUAL(f.set->tool->count, 0);
    BOOST_REQUIRE(f.state.turns[0].agent_loop_step[0].invoke_returns);
    BOOST_CHECK(
        f.state.turns[0].agent_loop_step[0].invoke_returns->at(0)
            .invoke_return->extras->at("loop_skipped").get<bool>());
}

BOOST_AUTO_TEST_CASE(hook_failure_after_response_closes_calls) {
    Fixture f;
    auto sub = f.bus.subscribe<loop::ModelCommitted>([](const auto&) {
        throw std::runtime_error("observer failed");
    });
    BOOST_CHECK(f.run().status == loop::RunStatus::Failed);
    BOOST_CHECK_EQUAL(f.set->tool->count, 0);
    BOOST_REQUIRE(f.state.turns[0].agent_loop_step[0].invoke_returns);
}

BOOST_AUTO_TEST_CASE(step_limit_settles_last_batch_and_continue_adds_no_input) {
    Fixture f;
    BOOST_CHECK(f.run(true, input(), 1).status == loop::RunStatus::StepLimit);
    BOOST_REQUIRE(f.state.turns[0].agent_loop_step[0].invoke_returns);
    BOOST_CHECK(f.run(false).status == loop::RunStatus::Completed);
    BOOST_CHECK_EQUAL(f.state.turns.size(), 1u);
    BOOST_CHECK_EQUAL(f.set->tool->count, 1);
}

BOOST_AUTO_TEST_CASE(model_failure_and_finish_failure_are_visible) {
    Fixture f;
    f.model.fail_model = true;
    auto sub = f.bus.subscribe<loop::RunFinished>([](const auto&) {
        throw std::runtime_error("finish failed");
    });
    const auto result = f.run();
    BOOST_CHECK(result.status == loop::RunStatus::Failed);
    BOOST_CHECK(result.error.find("model failure") != std::string::npos);
    BOOST_CHECK(result.error.find("finish failed") != std::string::npos);
    BOOST_CHECK_EQUAL(f.state.loop->error, result.error);
    BOOST_CHECK_EQUAL(f.state.turns.size(), 1u);
}

BOOST_AUTO_TEST_CASE(invalid_request_and_precancel_preserve_state) {
    Fixture f;
    const auto original = nlohmann::json(f.state);
    BOOST_CHECK(f.run(false).status == loop::RunStatus::Failed);
    BOOST_CHECK(nlohmann::json(f.state) == original);
    BOOST_CHECK(f.run(true, input(), 0).status == loop::RunStatus::Failed);
    std::stop_source source;
    source.request_stop();
    BOOST_CHECK(f.run(true, input(), 5, source.get_token()).status == loop::RunStatus::Cancelled);
    BOOST_CHECK(nlohmann::json(f.state) == original);
}

BOOST_AUTO_TEST_CASE(imported_unanswered_calls_are_not_replayed) {
    Fixture f;
    Item response;
    response.type = Kind::ModelResponse;
    model_io::InvokeQuery q;
    q.id = "old";
    q.name = "effect";
    response.invokes = std::vector{q};
    f.model.integrate(f.state, input());
    f.model.integrate(f.state, response);
    BOOST_CHECK(f.run(false).status == loop::RunStatus::Failed);
    BOOST_CHECK_EQUAL(f.set->tool->count, 0);
}

BOOST_AUTO_TEST_CASE(parallel_batch_is_joined_on_stop) {
    Fixture f;
    std::stop_source source;
    f.model.call_count = 2;
    f.set->tool->parallel = true;
    f.set->tool->after_effect = [&] {
        source.request_stop();
    };
    BOOST_CHECK(f.run(true, input(), 5, source.get_token()).status == loop::RunStatus::Cancelled);
    BOOST_CHECK_EQUAL(f.set->tool->count, 2);
    BOOST_REQUIRE(f.state.turns[0].agent_loop_step[0].invoke_returns);
    BOOST_CHECK_EQUAL(f.state.turns[0].agent_loop_step[0].invoke_returns->size(), 2u);
}

BOOST_AUTO_TEST_CASE(tool_failure_is_a_record_and_loop_continues) {
    Fixture f;
    f.set->tool->after_effect = [] {
        throw std::runtime_error("failed after effect");
    };
    BOOST_CHECK(f.run().status == loop::RunStatus::Completed);
    BOOST_CHECK_EQUAL(f.set->tool->count, 1);
    const auto& record = f.state.turns[0].agent_loop_step[0].invoke_returns->at(0);
    BOOST_CHECK(tools::is_error(*record.invoke_return));
    BOOST_CHECK(record.content[0].raw.find("failed after effect") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(invalid_input_hook_does_not_append_turn) {
    Fixture f;
    auto sub = f.bus.subscribe<loop::BeforeInput>([](const auto& e) {
        e.input.type = Kind::ModelResponse;
    });
    BOOST_CHECK(f.run().status == loop::RunStatus::Failed);
    BOOST_CHECK(f.state.turns.empty());
}

BOOST_AUTO_TEST_CASE(old_json_and_blocked_recovery) {
    Fixture f;
    auto json = nlohmann::json(f.state);
    BOOST_CHECK(!json.contains("loop"));
    f.state = json.get<State>();
    f.state.loop = model_io::LoopProgress{};
    f.state.loop->phase = model_io::LoopPhase::Tools;
    const auto original = nlohmann::json(f.state);
    BOOST_CHECK(f.run().status == loop::RunStatus::Failed);
    BOOST_CHECK(nlohmann::json(f.state) == original);
    BOOST_CHECK_EQUAL(f.set->tool->count, 0);
}

/**
 * A cancellable provider whose timer represents a stalled network request.
 * The RAII marker proves that run() waits for the model frame to unwind.
 */
struct SuspendedModel : Model {
    asio::steady_timer wait;
    std::promise<void> entered;
    bool unwound = false;
    bool suspend = true;

    explicit SuspendedModel(asio::any_io_executor executor)
        : Model(executor), wait(executor) {
        calls = false;
    }

    asio::awaitable<Item> converse(State state) override {
        if (suspend) {
            struct ExitMarker {
                bool& unwound;
                ~ExitMarker() {
                    unwound = true;
                }
            } marker{unwound};

            wait.expires_at(asio::steady_timer::time_point::max());
            entered.set_value();
            co_await wait.async_wait(asio::use_awaitable);
        }
        co_return co_await Model::converse(std::move(state));
    }
};

BOOST_AUTO_TEST_CASE(stop_interrupts_suspended_model_and_allows_continuation) {
    asio::io_context io;
    SuspendedModel model(io.get_executor());
    tools::ToolRegistry registry;
    eventbus::EventBus bus;
    State state;
    std::stop_source stop;
    auto entered = model.entered.get_future();
    auto result = asio::co_spawn(io, loop::run(
        model, registry, bus, io.get_executor(), state, true, input(), {},
        stop.get_token()), asio::use_future);

    // Two executor workers exercise cross-thread stop delivery. The model's
    // entered signal gives a deterministic point before its indefinite wait.
    std::thread first([&] { io.run(); });
    std::thread second([&] { io.run(); });
    const bool started = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    stop.request_stop();
    const bool finished = result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!finished) {
        io.stop();
    }
    first.join();
    second.join();
    BOOST_REQUIRE(started);
    BOOST_REQUIRE(finished);
    BOOST_CHECK(result.get().status == loop::RunStatus::Cancelled);
    BOOST_CHECK(model.unwound);
    BOOST_CHECK_EQUAL(state.turns.size(), 1u);
    BOOST_CHECK(state.turns[0].agent_loop_step.empty());
    BOOST_CHECK(state.loop->phase == model_io::LoopPhase::Ready);

    model.suspend = false;
    io.restart();
    auto next = asio::co_spawn(io, loop::run(
        model, registry, bus, io.get_executor(), state, false, {}), asio::use_future);
    io.run();
    BOOST_CHECK(next.get().status == loop::RunStatus::Completed);
    BOOST_CHECK_EQUAL(state.turns.size(), 1u);
}

BOOST_AUTO_TEST_CASE(model_error_is_not_hidden_by_a_simultaneous_stop) {
    Fixture f;
    std::stop_source stop;
    f.model.inspect = [&](const State&) {
        stop.request_stop();
        throw std::runtime_error("independent model error");
    };
    const auto result = f.run(true, input(), 5, stop.get_token());
    BOOST_CHECK(result.status == loop::RunStatus::Failed);
    BOOST_CHECK(result.error.find("independent model error") != std::string::npos);
}
