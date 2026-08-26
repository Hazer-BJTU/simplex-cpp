#define BOOST_TEST_MODULE chat_completions_interpreter
#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include <memory>

#include "endpoint/http_request_exception.hpp"
#include "llm/chat_completions/interpreter.hpp"

namespace http = boost::beast::http;
using llm::chat_completions::ChatCompletionsInterpreter;

namespace {

class TestDialect final
    : public llm::chat_completions::ChatCompletionsDialect {
public:
    void transform_request(nlohmann::json& body) const override {
        body["provider_extension"] = "enabled";
    }
};

// The thinking-mode provider stance: assistant reasoning is replayed.
class ReplayingDialect final
    : public llm::chat_completions::ChatCompletionsDialect {
public:
    bool replay_assistant_reasoning() const override { return true; }
};

struct Fixture {
    model_io::ModelEndpoint endpoint;
    nlohmann::json generation{{"model", "chat-test"}};

    Fixture() {
        endpoint.base_url = "https://chat.example.com:8443";
        endpoint.request_path = "/openai/v1/chat/completions";
        endpoint.auth.scheme = model_io::AuthScheme::None;
    }
};

nlohmann::json body_of(
    const endpoint::ModelRequestInterpreter::HttpRequest& request) {
    return nlohmann::json::parse(request.body());
}

model_io::AgentInputState react_state() {
    model_io::AgentInputState state;
    state.system_prompt.add_section("identity", "Identity", "Be concise.");

    model_io::Invocable weather;
    weather.name = "weather";
    weather.description = "Get the weather";
    weather.argument_schema = {
        {"type", "object"},
        {"properties", {{"city", {{"type", "string"}}}}},
    };
    state.tools.push_back(weather);

    model_io::UserLoopStep turn;
    turn.user_input.role = "user";
    turn.user_input.content.push_back(
        {model_io::ContentType::Text, "Weather in Paris?"});

    model_io::AgentLoopStep call_step;
    call_step.model_response.type =
        model_io::MessageItemType::ModelResponse;
    call_step.model_response.role = "assistant";
    model_io::InvokeQuery query;
    query.id = "call_1";
    query.name = "weather";
    query.arguments = {{"city", "Paris"}};
    call_step.model_response.invokes =
        std::vector<model_io::InvokeQuery>{query};

    model_io::MessageItem tool_result;
    tool_result.type = model_io::MessageItemType::InvokeReturn;
    tool_result.role = "tool";
    tool_result.content.push_back(
        {model_io::ContentType::Text, "sunny"});
    model_io::InvokeReturn provenance;
    provenance.query = query;
    provenance.output = {model_io::ContentType::Text, "sunny"};
    tool_result.invoke_return = provenance;
    call_step.invoke_returns =
        std::vector<model_io::MessageItem>{tool_result};
    turn.agent_loop_step.push_back(call_step);

    model_io::AgentLoopStep final_step;
    final_step.model_response.type =
        model_io::MessageItemType::ModelResponse;
    final_step.model_response.role = "assistant";
    final_step.model_response.content.push_back(
        {model_io::ContentType::Text, "It is sunny."});
    turn.agent_loop_step.push_back(final_step);
    state.turns.push_back(turn);
    return state;
}

} // namespace

BOOST_AUTO_TEST_CASE(empty_state_builds_streaming_single_choice_request) {
    Fixture fixture;
    ChatCompletionsInterpreter interpreter;
    const auto request = interpreter.build_request(
        {}, fixture.endpoint, fixture.generation);
    const auto body = body_of(request);

    BOOST_CHECK(request.method() == http::verb::post);
    BOOST_CHECK_EQUAL(request.target(), "/openai/v1/chat/completions");
    BOOST_CHECK_EQUAL(std::string(request.at(http::field::host)),
                      "chat.example.com:8443");
    BOOST_CHECK_EQUAL(std::string(request.at(http::field::accept)),
                      "text/event-stream");
    BOOST_CHECK_EQUAL(body["model"], "chat-test");
    BOOST_CHECK(body["messages"].empty());
    BOOST_CHECK_EQUAL(body["stream"], true);
    BOOST_CHECK_EQUAL(body["n"], 1);
    BOOST_CHECK(!body.contains("tools"));
}

BOOST_AUTO_TEST_CASE(react_history_maps_to_chat_messages_and_function_tools) {
    Fixture fixture;
    ChatCompletionsInterpreter interpreter;
    const auto body = body_of(interpreter.build_request(
        react_state(), fixture.endpoint, fixture.generation));

    BOOST_REQUIRE_EQUAL(body["messages"].size(), 5u);
    BOOST_CHECK_EQUAL(body["messages"][0]["role"], "system");
    BOOST_CHECK_NE(body["messages"][0]["content"].get<std::string>().find(
                       "Be concise."),
                   std::string::npos);
    BOOST_CHECK_EQUAL(body["messages"][1]["role"], "user");
    BOOST_CHECK_EQUAL(body["messages"][1]["content"], "Weather in Paris?");

    const auto& assistant = body["messages"][2];
    BOOST_CHECK_EQUAL(assistant["role"], "assistant");
    BOOST_CHECK(assistant["content"].is_null());
    BOOST_CHECK_EQUAL(assistant["tool_calls"][0]["id"], "call_1");
    BOOST_CHECK_EQUAL(assistant["tool_calls"][0]["type"], "function");
    BOOST_CHECK_EQUAL(assistant["tool_calls"][0]["function"]["name"],
                      "weather");
    BOOST_CHECK_EQUAL(assistant["tool_calls"][0]["function"]["arguments"],
                      "{\"city\":\"Paris\"}");

    BOOST_CHECK_EQUAL(body["messages"][3]["role"], "tool");
    BOOST_CHECK_EQUAL(body["messages"][3]["tool_call_id"], "call_1");
    BOOST_CHECK_EQUAL(body["messages"][3]["content"], "sunny");
    BOOST_CHECK_EQUAL(body["messages"][4]["content"], "It is sunny.");

    BOOST_REQUIRE_EQUAL(body["tools"].size(), 1u);
    BOOST_CHECK_EQUAL(body["tools"][0]["type"], "function");
    BOOST_CHECK_EQUAL(body["tools"][0]["function"]["name"], "weather");
    BOOST_CHECK_EQUAL(body["tools"][0]["function"]["description"],
                      "Get the weather");
}

BOOST_AUTO_TEST_CASE(parallel_calls_and_results_keep_wire_order) {
    Fixture fixture;
    ChatCompletionsInterpreter interpreter;
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content.push_back(
        {model_io::ContentType::Text, "compare"});
    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    model_io::InvokeQuery first;
    first.id = "a";
    first.name = "lookup";
    first.arguments = {{"x", 1}};
    model_io::InvokeQuery second;
    second.id = "b";
    second.name = "lookup";
    second.arguments = {{"x", 2}};
    step.model_response.invokes =
        std::vector<model_io::InvokeQuery>{first, second};

    // No embedded provenance: exact cardinality enables positional fallback.
    step.invoke_returns = std::vector<model_io::MessageItem>(2);
    (*step.invoke_returns)[0].content.push_back(
        {model_io::ContentType::Text, "one"});
    (*step.invoke_returns)[1].content.push_back(
        {model_io::ContentType::Text, "two"});
    turn.agent_loop_step.push_back(step);
    state.turns.push_back(turn);

    const auto messages = body_of(interpreter.build_request(
        state, fixture.endpoint, fixture.generation))["messages"];
    BOOST_REQUIRE_EQUAL(messages[1]["tool_calls"].size(), 2u);
    BOOST_CHECK_EQUAL(messages[2]["tool_call_id"], "a");
    BOOST_CHECK_EQUAL(messages[3]["tool_call_id"], "b");
}

BOOST_AUTO_TEST_CASE(multimodal_user_content_uses_chat_content_parts) {
    Fixture fixture;
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content.push_back(
        {model_io::ContentType::Text, "describe"});
    turn.user_input.content.push_back(
        {model_io::ContentType::ExternalRef,
         "https://example.com/cat.png",
         nlohmann::json{{"detail", "low"}}});
    state.turns.push_back(turn);

    const auto content = body_of(ChatCompletionsInterpreter{}.build_request(
        state, fixture.endpoint, fixture.generation))["messages"][0]["content"];
    BOOST_REQUIRE_EQUAL(content.size(), 2u);
    BOOST_CHECK_EQUAL(content[0]["type"], "text");
    BOOST_CHECK_EQUAL(content[1]["type"], "image_url");
    BOOST_CHECK_EQUAL(content[1]["image_url"]["url"],
                      "https://example.com/cat.png");
    BOOST_CHECK_EQUAL(content[1]["image_url"]["detail"], "low");
}

BOOST_AUTO_TEST_CASE(generation_passthrough_but_builder_owned_keys_win) {
    Fixture fixture;
    fixture.generation = {
        {"model", "chat-test"},
        {"temperature", 0.25},
        {"stream", false},
        {"n", 9},
        {"messages", nlohmann::json::array({{{"role", "user"}}})},
        {"tools", nlohmann::json::array({{{"type", "stale"}}})},
    };
    const auto body = body_of(ChatCompletionsInterpreter{}.build_request(
        {}, fixture.endpoint, fixture.generation));
    BOOST_CHECK_EQUAL(body["temperature"], 0.25);
    BOOST_CHECK_EQUAL(body["stream"], true);
    BOOST_CHECK_EQUAL(body["n"], 1);
    BOOST_CHECK(body["messages"].empty());
    BOOST_CHECK(!body.contains("tools"));
}

BOOST_AUTO_TEST_CASE(missing_model_and_host_are_create_request_errors) {
    Fixture fixture;
    ChatCompletionsInterpreter interpreter;
    BOOST_CHECK_THROW(
        interpreter.build_request({}, fixture.endpoint, nlohmann::json::object()),
        HttpRequestException);
    fixture.endpoint.base_url.clear();
    BOOST_CHECK_THROW(
        interpreter.build_request({}, fixture.endpoint, fixture.generation),
        HttpRequestException);
}

BOOST_AUTO_TEST_CASE(dialect_can_rewrite_the_finished_request) {
    Fixture fixture;
    ChatCompletionsInterpreter interpreter(std::make_shared<TestDialect>());
    const auto body = body_of(interpreter.build_request(
        {}, fixture.endpoint, fixture.generation));
    BOOST_CHECK_EQUAL(body["provider_extension"], "enabled");
}

BOOST_AUTO_TEST_CASE(assistant_refusal_is_replayed_as_refusal_not_text) {
    Fixture fixture;
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content.push_back(
        {model_io::ContentType::Text, "unsafe request"});

    model_io::AgentLoopStep refusal_step;
    refusal_step.model_response.type =
        model_io::MessageItemType::ModelResponse;
    refusal_step.model_response.role = "assistant";
    refusal_step.model_response.content.push_back({
        model_io::ContentType::Text,
        "cannot comply",
        nlohmann::json{{"refusal", "cannot comply"}},
    });
    turn.agent_loop_step.push_back(std::move(refusal_step));
    state.turns.push_back(std::move(turn));

    const auto messages = body_of(ChatCompletionsInterpreter{}.build_request(
        state, fixture.endpoint, fixture.generation))["messages"];
    BOOST_REQUIRE_EQUAL(messages.size(), 2u);
    BOOST_CHECK(messages[1]["content"].is_null());
    BOOST_CHECK_EQUAL(messages[1]["refusal"], "cannot comply");
}

// --- the shared envelope, the usage trailer, and reasoning replay --------------

model_io::AgentInputState reasoning_state() {
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content.push_back(
        {model_io::ContentType::Text, "think hard"});

    model_io::AgentLoopStep step;
    step.model_response.type =
        model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.content.push_back(
        {model_io::ContentType::Text, "the answer"});
    model_io::Content reasoning;
    reasoning.type = model_io::ContentType::Text;
    reasoning.raw = "chain of thought";
    step.model_response.reasoning = std::move(reasoning);
    turn.agent_loop_step.push_back(std::move(step));
    state.turns.push_back(std::move(turn));
    return state;
}

BOOST_AUTO_TEST_CASE(stream_options_include_usage_is_builder_owned) {
    Fixture fixture;
    fixture.generation = {
        {"model", "chat-test"},
        {"stream_options",
         {{"include_usage", false}, {"continuous_usage_stats", true}}},
    };
    const auto body = body_of(ChatCompletionsInterpreter{}.build_request(
        {}, fixture.endpoint, fixture.generation));
    // The reader's cost accounting lives on the usage trailer, so a stale
    // false cannot turn it off — but key-level setting keeps siblings.
    BOOST_CHECK_EQUAL(body["stream_options"]["include_usage"], true);
    BOOST_CHECK_EQUAL(body["stream_options"]["continuous_usage_stats"], true);

    const auto plain = body_of(ChatCompletionsInterpreter{}.build_request(
        {}, fixture.endpoint, Fixture{}.generation));
    BOOST_CHECK_EQUAL(plain["stream_options"]["include_usage"], true);
}

BOOST_AUTO_TEST_CASE(reasoning_effort_translates_from_the_shared_config_envelope) {
    Fixture fixture;
    fixture.generation = {
        {"model", "chat-test"},
        {"reasoning", {{"effort", "medium"}}},
    };
    const auto body = body_of(ChatCompletionsInterpreter{}.build_request(
        {}, fixture.endpoint, fixture.generation));
    BOOST_CHECK_EQUAL(body["reasoning_effort"], "medium");
    // The envelope object has no chat-completions wire form; consumed.
    BOOST_CHECK(!body.contains("reasoning"));
}

BOOST_AUTO_TEST_CASE(explicit_reasoning_effort_wins_and_the_envelope_is_erased) {
    Fixture fixture;
    fixture.generation = {
        {"model", "chat-test"},
        {"reasoning", {{"effort", "low"}}},
        {"reasoning_effort", "high"},
    };
    const auto body = body_of(ChatCompletionsInterpreter{}.build_request(
        {}, fixture.endpoint, fixture.generation));
    BOOST_CHECK_EQUAL(body["reasoning_effort"], "high");
    BOOST_CHECK(!body.contains("reasoning"));
}

BOOST_AUTO_TEST_CASE(reasoning_envelope_without_effort_is_erased) {
    Fixture fixture;
    fixture.generation = {
        {"model", "chat-test"},
        {"reasoning", {{"summary", "auto"}}},
    };
    const auto body = body_of(ChatCompletionsInterpreter{}.build_request(
        {}, fixture.endpoint, fixture.generation));
    BOOST_CHECK(!body.contains("reasoning"));
    BOOST_CHECK(!body.contains("reasoning_effort"));
}

BOOST_AUTO_TEST_CASE(assistant_reasoning_replays_only_when_the_dialect_opts_in) {
    Fixture fixture;
    const auto state = reasoning_state();

    const auto neutral = body_of(ChatCompletionsInterpreter{}.build_request(
        state, fixture.endpoint, fixture.generation))["messages"];
    // Strict servers reject the unknown field: the default stays off.
    BOOST_CHECK(!neutral[1].contains("reasoning_content"));

    const auto replaying =
        body_of(ChatCompletionsInterpreter(std::make_shared<ReplayingDialect>())
                    .build_request(state, fixture.endpoint,
                                   fixture.generation))["messages"];
    BOOST_CHECK_EQUAL(replaying[1]["reasoning_content"], "chain of thought");
}
