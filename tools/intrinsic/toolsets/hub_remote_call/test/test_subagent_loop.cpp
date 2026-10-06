#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast.hpp>

#include "loop/loop.hpp"
#include "loop/events.hpp"
#include "eventbus/event_bus.hpp"
#include "llm/models.hpp"
#include "tools/registry.hpp"
#include "tools/intrinsic/hub_remote_call/toolset.hpp"

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using Json = nlohmann::json;

/** A deterministic driver requests exactly one fork before its final answer. */
class Model final : public llm::LLMModel {
public:
    explicit Model(asio::any_io_executor executor) : LLMModel(executor, {}) {}
    int calls = 0;

    llm::LLMModelType model_type() const noexcept override
    {
        return llm::LLMModelType::Conversation;
    }

    asio::awaitable<model_io::MessageItem> converse(model_io::AgentInputState) override
    {
        model_io::MessageItem response;
        response.type = model_io::MessageItemType::ModelResponse;
        response.role = "assistant";
        if (++calls == 1) {
            model_io::InvokeQuery call;
            call.id = "fork-call";
            call.name = "subagent_fork";
            call.arguments = Json::object();
            response.invokes = std::vector{call};
        } else {
            response.content.push_back({model_io::ContentType::Text, "continued"});
        }
        co_return response;
    }
};
} // namespace

BOOST_AUTO_TEST_CASE(cancelled_loop_commits_the_fork_result_then_continues_without_repeating_it)
{
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    tools::ToolRegistry registry;
    registry.add(std::make_shared<tools::intrinsic::HubRemoteCallToolSet>(
        endpoint::ResolvedEndpoint{"127.0.0.1", std::to_string(acceptor.local_endpoint().port()),
            "/agent/parent/tools", false},
        std::chrono::milliseconds(2000), [] {
            return tools::intrinsic::HubRemoteCallIdentity{"worker", "parent", "run"};
        }));
    Model model(io.get_executor());
    eventbus::EventBus bus;
    model_io::AgentInputState state;
    std::stop_source stop;
    int requests = 0;
    bool committed = false;
    auto subscription = bus.subscribe<loop::ToolResultsCommitted>([&](const auto&) {
        committed = true;
    });
    const auto peer = [&]() -> asio::awaitable<void> {
        ws::stream<asio::ip::tcp::socket> socket(co_await acceptor.async_accept(asio::use_awaitable));
        co_await socket.async_accept(asio::use_awaitable);
        beast::flat_buffer buffer;
        co_await socket.async_read(buffer, asio::use_awaitable);
        ++requests;
        auto data = Json::parse(beast::buffers_to_string(buffer.data())).at("data");
        // Simulate a mutation already committed remotely when the parent cancels.
        stop.request_stop();
        asio::steady_timer timer(io, std::chrono::milliseconds(20));
        co_await timer.async_wait(asio::use_awaitable);
        data.erase("arguments");
        data["route"] = "subagent/clean-fork";
        data["status"] = "succeeded";
        data["result"] = {{"subagent_id", "subagent-created"}, {"lifecycle", "preparing"}};
        const auto reply = Json{{"type", "tool_response"}, {"data", data}}.dump();
        socket.text(true);
        co_await socket.async_write(asio::buffer(reply), asio::use_awaitable);
        boost::system::error_code error;
        co_await socket.async_read(buffer, asio::redirect_error(asio::use_awaitable, error));
    };
    auto server = asio::co_spawn(io, peer, asio::use_future);
    model_io::MessageItem message;
    message.role = "user";
    message.content.push_back({model_io::ContentType::Text, "delegate"});
    auto future = asio::co_spawn(io, loop::run(model, registry, bus, io.get_executor(),
        state, true, message, {}, stop.get_token()), asio::use_future);
    io.run();
    server.get();
    BOOST_CHECK(future.get().status == loop::RunStatus::Cancelled);
    BOOST_TEST(committed);
    BOOST_TEST(requests == 1);
    BOOST_TEST(model.calls == 1);
    BOOST_REQUIRE(state.turns.size() == 1u);
    BOOST_REQUIRE(state.turns[0].agent_loop_step[0].invoke_returns);
    BOOST_TEST(state.turns[0].agent_loop_step[0].invoke_returns->at(0).content.at(0).raw
        .find("subagent-created") != std::string::npos);
    io.restart();
    auto resumed = asio::co_spawn(io, loop::run(model, registry, bus, io.get_executor(),
        state, false, {}, {}), asio::use_future);
    io.run();
    BOOST_CHECK(resumed.get().status == loop::RunStatus::Completed);
    BOOST_TEST(model.calls == 2);
    BOOST_TEST(requests == 1);
}
