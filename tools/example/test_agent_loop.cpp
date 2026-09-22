#define BOOST_TEST_MODULE ToolsAgentLoopTests
#include <boost/test/unit_test.hpp>

#include "agent_loop.hpp"
#include "tools/intrinsic/process/toolset.hpp"
#include "tools/invoke_exception.hpp"
#include "tools/security_check.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <stdexcept>

namespace asio = boost::asio;
namespace {
model_io::MessageItem user_input(const std::string& text) {
    model_io::MessageItem item;
    item.type = model_io::MessageItemType::UserInput;
    item.role = "user";
    model_io::Content content;
    content.type = model_io::ContentType::Text;
    content.raw = text;
    item.content.push_back(content);
    return item;
}

// A provider which requests one real process, fails on its next exchange,
// then recovers. No network or API key is needed to exercise the example loop.
struct FailingProvider : llm::LLMModel {
    explicit FailingProvider(asio::any_io_executor executor)
        : LLMModel(executor, nlohmann::json::object()) {}
    llm::LLMModelType model_type() const noexcept override {
        return llm::LLMModelType::Conversation;
    }
    int exchanges = 0;
    bool fail_first = false;
    bool saw_previous_result = false;
    asio::awaitable<model_io::MessageItem> converse(
        model_io::AgentInputState state) override {
        const int exchange = exchanges++;
        if (fail_first || exchange == 1) throw std::runtime_error("provider failed");
        model_io::MessageItem response;
        response.type = model_io::MessageItemType::ModelResponse;
        response.role = "assistant";
        if (exchange == 0) {
            model_io::InvokeQuery query;
            query.id = "launch_once";
            query.name = "spawn_process";
            query.arguments = {{"executable", "echo"}, {"arguments", {"already-executed"}}};
            response.invokes = std::vector<model_io::InvokeQuery>{query};
        } else {
            saw_previous_result = state.turns.size() == 2 &&
                state.turns[0].agent_loop_step.size() == 1 &&
                state.turns[0].agent_loop_step[0].invoke_returns.has_value();
        }
        co_return response;
    }
};

template<class T>
T run(asio::io_context& io, asio::awaitable<T> task) {
    auto future = asio::co_spawn(io, std::move(task), asio::use_future);
    io.restart();
    io.run();
    return future.get();
}
}

BOOST_AUTO_TEST_CASE(provider_failure_preserves_executed_tools_for_the_next_turn)
{
    asio::io_context io;
    eventbus::AsyncEventBus bus;
    auto approval = bus.subscribe<tools::InvokeConfirmEvent>(
        [](tools::InvokeConfirmEvent event) -> asio::awaitable<tools::InvokeConfirmEvent> {
            event.decision = tools::ConfirmDecision::Approved;
            co_return event;
        });
    auto store = std::make_shared<tools::intrinsic::ProcessSessionStore>(io.get_executor());
    auto set = std::make_shared<tools::intrinsic::ProcessToolSet>(store, &bus);
    tools::ToolRegistry registry;
    registry.add(set);
    FailingProvider model(io.get_executor());
    model_io::AgentInputState state;
    BOOST_TEST(!run(io, tools_example::run_user_turn(
        model, registry, state, user_input("run once"), 5)));
    BOOST_REQUIRE_EQUAL(state.turns.size(), 1u);
    BOOST_REQUIRE_EQUAL(state.turns[0].agent_loop_step.size(), 1u);
    const auto& results = state.turns[0].agent_loop_step[0].invoke_returns;
    BOOST_REQUIRE(results.has_value());
    BOOST_REQUIRE_EQUAL(results->size(), 1u);
    BOOST_REQUIRE(results->front().invoke_return.has_value());
    BOOST_TEST(!tools::is_error(*results->front().invoke_return));
    BOOST_TEST(results->front().content.at(0).raw.find("already-executed") != std::string::npos);
    BOOST_TEST(run(io, store->size()) == 1u);

    BOOST_TEST(run(io, tools_example::run_user_turn(
        model, registry, state, user_input("continue"), 5)));
    BOOST_TEST(model.saw_previous_result);
    BOOST_TEST(run(io, store->size()) == 1u); // No duplicate launch on recovery.
    approval.disconnect();
}

BOOST_AUTO_TEST_CASE(first_exchange_failure_keeps_the_submitted_input)
{
    asio::io_context io;
    FailingProvider model(io.get_executor());
    model.fail_first = true;
    tools::ToolRegistry registry;
    model_io::AgentInputState state;
    BOOST_TEST(!run(io, tools_example::run_user_turn(
        model, registry, state, user_input("keep this request"), 5)));
    BOOST_REQUIRE_EQUAL(state.turns.size(), 1u);
    BOOST_TEST(state.turns[0].agent_loop_step.empty());
    BOOST_TEST(state.turns[0].user_input.content.at(0).raw == "keep this request");
}
