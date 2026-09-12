#define BOOST_TEST_MODULE tool_result_correlation
#include <boost/test/unit_test.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "llm/chat_completions/interpreter.hpp"
#include "tools/registry.hpp"

// The correlation the tool layer owes the WIRE, tested where it is spent rather
// than where it is produced.
//
// A tool record's query.id is not diagnostics: ChatCompletionsInterpreter::
// emit_tool_results() emits it as the tool message's tool_call_id. So a record
// produced for the model's call_1 must reach the request as tool_call_id
// "call_1" — even when the tool failed on a nested call of its own making and
// the failure carried that call's id. Getting this wrong leaves the model's own
// tool call unanswered, and the provider rejects the whole turn.
//
// Everything above the assertion is real: a real ToolRegistry runs a real batch
// through a real ToolSet and ToolInterface, and the records it answers with go
// through the real request builder.

namespace asio = boost::asio;
using llm::chat_completions::ChatCompletionsInterpreter;
using tools::InvokeException;

namespace {

model_io::InvokeQuery call_for(std::string name, std::string id)
{
    model_io::InvokeQuery query;
    query.type = model_io::InvokeType::ReadOnly;
    query.security = model_io::InvokeSecurity::DefaultDeny;
    query.id = std::move(id);
    query.name = std::move(name);
    return query;
}

/// A tool that always fails on a call of its OWN making: the nested invocation
/// shape — a delegated call, a retry, an internal fetch — and the one that used
/// to be able to take over the record's identity.
class NestedFailureTool final : public tools::ToolInterface {
public:
    NestedFailureTool()
    {
        details.name = "read_file";
        details.description = "Reads a file, badly.";
    }

    model_io::Invocable details;

    const model_io::Invocable& get_details() const noexcept override
    {
        return details;
    }

    void write_attributes(model_io::InvokeQuery& query) const override
    {
        query.type = model_io::InvokeType::SerialWrite;
        query.security = model_io::InvokeSecurity::Trusted;
    }

    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery& query) override
    {
        model_io::InvokeQuery nested = query;
        nested.id = "inner_call";
        nested.name = "fetch_url";
        throw InvokeException(
            InvokeException::Stage::Invoke,
            "the upstream fetch failed",
            std::move(nested));

        co_return model_io::Content{};   // unreachable; keeps the coroutine a coroutine
    }
};

/// A tool that answers normally, so the batch also carries a record with nothing
/// interesting about it.
class QuietTool final : public tools::ToolInterface {
public:
    QuietTool()
    {
        details.name = "stat_file";
        details.description = "Reports a file's size.";
    }

    model_io::Invocable details;

    const model_io::Invocable& get_details() const noexcept override
    {
        return details;
    }

    void write_attributes(model_io::InvokeQuery& query) const override
    {
        query.type = model_io::InvokeType::ReadOnly;
        query.security = model_io::InvokeSecurity::Trusted;
    }

    boost::asio::awaitable<model_io::Content> invoke(
        const model_io::InvokeQuery&) override
    {
        co_return model_io::Content{
            .type = model_io::ContentType::Text, .raw = "12 bytes", .extras = {}};
    }
};

/// A set of the two tools above, resolving by name.
class TwoToolSet final : public tools::ToolSet {
public:
    TwoToolSet()
        : tools_{std::make_shared<NestedFailureTool>(), std::make_shared<QuietTool>()}
    {}

    std::string_view name() const noexcept override { return "local_tools"; }

    std::vector<model_io::Invocable> get_tools() const override
    {
        return {tools_[0]->get_details(), tools_[1]->get_details()};
    }

    ToolHandle dispatch(const model_io::InvokeQuery& query) const override
    {
        for (const ToolHandle& tool : tools_) {
            if (tool->get_details().name == query.name) return tool;
        }
        return nullptr;
    }

private:
    std::vector<ToolHandle> tools_;
};

/// Run a batch and hand back the records — the agent loop's own call.
tools::ToolRegistry::Results run_batch(
    const tools::ToolRegistry& registry, std::vector<model_io::InvokeQuery> batch)
{
    asio::io_context io;
    auto pending = asio::co_spawn(
        io, registry.execute(std::move(batch), io.get_executor()), asio::use_future);
    io.run();
    return pending.get();
}

struct Fixture {
    model_io::ModelEndpoint endpoint;
    nlohmann::json generation{{"model", "chat-test"}};

    Fixture()
    {
        endpoint.base_url = "https://chat.example.com:8443";
        endpoint.request_path = "/openai/v1/chat/completions";
        endpoint.auth.scheme = model_io::AuthScheme::None;
    }
};

} // namespace

BOOST_AUTO_TEST_CASE(a_nested_failure_still_answers_the_models_own_tool_call)
{
    tools::ToolRegistry registry;
    registry.add(std::make_shared<TwoToolSet>());

    // The batch the model asked for: two calls, one of which fails internally on
    // a call the model never made.
    const tools::ToolRegistry::Results records = run_batch(registry, {
        call_for("read_file", "call_1"),
        call_for("stat_file", "call_2"),
    });
    BOOST_REQUIRE_EQUAL(records.size(), 2u);

    // The tool layer's own guarantee, before the wire is involved...
    BOOST_TEST(records[0].query.id == "call_1");
    BOOST_TEST(tools::is_error(records[0]));
    BOOST_REQUIRE(records[0].extras.has_value());
    // ... with the nested call preserved rather than lost.
    BOOST_TEST(records[0].extras->at("cause_query").at("id") == "inner_call");
    BOOST_TEST(records[0].extras->at("cause_query").at("name") == "fetch_url");
    BOOST_TEST(records[1].query.id == "call_2");
    BOOST_TEST(!tools::is_error(records[1]));

    // The turn as the agent loop assembles it: the assistant message that made
    // the calls, then one tool result per call — content left empty, so the
    // interpreter falls back to the record's prose, exactly as a host that
    // forwards a failure does.
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.role = "user";
    turn.user_input.content.push_back(model_io::Content{
        .type = model_io::ContentType::Text,
        .raw = "How big is /etc/hosts?",
        .extras = {}});

    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.invokes = std::vector<model_io::InvokeQuery>{
        call_for("read_file", "call_1"), call_for("stat_file", "call_2")};

    std::vector<model_io::MessageItem> results;
    for (const model_io::InvokeReturn& record : records) {
        model_io::MessageItem item;
        item.type = model_io::MessageItemType::InvokeReturn;
        item.role = "tool";
        item.invoke_return = record;
        results.push_back(std::move(item));
    }
    step.invoke_returns = std::move(results);
    turn.agent_loop_step.push_back(step);
    state.turns.push_back(turn);

    Fixture fixture;
    ChatCompletionsInterpreter interpreter;
    const endpoint::ModelRequestInterpreter::HttpRequest request =
        interpreter.build_request(state, fixture.endpoint, fixture.generation);
    const nlohmann::json body = nlohmann::json::parse(request.body());

    // The request carries the model's own two calls...
    const nlohmann::json& assistant = body["messages"][1];
    BOOST_REQUIRE_EQUAL(assistant["tool_calls"].size(), 2u);
    BOOST_TEST(assistant["tool_calls"][0]["id"] == "call_1");
    BOOST_TEST(assistant["tool_calls"][1]["id"] == "call_2");

    // ... and EVERY one of them is answered, by a tool message carrying exactly
    // that id. This is the assertion the nested failure would break: with the
    // nested call taking over the record's identity, this message would say
    // tool_call_id "inner_call" and the provider would reject the turn for an
    // unanswered call_1.
    BOOST_REQUIRE(body["messages"].size() >= 4u);
    const nlohmann::json& failed = body["messages"][2];
    const nlohmann::json& answered = body["messages"][3];
    BOOST_TEST(failed["role"] == "tool");
    BOOST_TEST(failed["tool_call_id"] == "call_1");
    BOOST_TEST(answered["role"] == "tool");
    BOOST_TEST(answered["tool_call_id"] == "call_2");

    // What the model reads is the failure, as prose the tool layer rendered.
    BOOST_TEST(failed["content"] ==
               "Failed while invoking the tool: the upstream fetch failed "
               "(tool read_file; call call_1)");
    BOOST_TEST(answered["content"] == "12 bytes");

    // Nothing on the wire names the inner call: it is host-side causal context,
    // not a tool call the provider ever issued.
    BOOST_TEST(body.dump().find("inner_call") == std::string::npos);
    BOOST_TEST(body.dump().find("fetch_url") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_unroutable_call_is_answered_in_the_request_too)
{
    // The same guarantee for the failure that never reaches a tool: the registry
    // answers an unknown name with a Dispatch record, and that record has to
    // become a tool message for the model's call like any other — otherwise the
    // turn is malformed for a reason the model cannot even see.
    tools::ToolRegistry registry;
    registry.add(std::make_shared<TwoToolSet>());

    const tools::ToolRegistry::Results records =
        run_batch(registry, {call_for("rm_rf", "call_9")});
    BOOST_REQUIRE_EQUAL(records.size(), 1u);
    BOOST_TEST(tools::is_error(records[0]));

    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.role = "user";
    turn.user_input.content.push_back(model_io::Content{
        .type = model_io::ContentType::Text, .raw = "clean up", .extras = {}});

    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.invokes = std::vector<model_io::InvokeQuery>{
        call_for("rm_rf", "call_9")};

    model_io::MessageItem item;
    item.type = model_io::MessageItemType::InvokeReturn;
    item.role = "tool";
    item.invoke_return = records[0];
    step.invoke_returns = std::vector<model_io::MessageItem>{item};
    turn.agent_loop_step.push_back(step);
    state.turns.push_back(turn);

    Fixture fixture;
    ChatCompletionsInterpreter interpreter;
    const nlohmann::json body = nlohmann::json::parse(
        interpreter.build_request(state, fixture.endpoint, fixture.generation).body());

    BOOST_TEST(body["messages"][1]["tool_calls"][0]["id"] == "call_9");
    BOOST_TEST(body["messages"][2]["role"] == "tool");
    BOOST_TEST(body["messages"][2]["tool_call_id"] == "call_9");
    BOOST_TEST(body["messages"][2]["content"] ==
               "Failed while dispatching the invocation to a tool: no toolset in "
               "the registry provides a tool named \"rm_rf\" (tool rm_rf; call call_9)");
}
