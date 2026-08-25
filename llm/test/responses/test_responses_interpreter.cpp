// Offline tests for the Responses-API request interpreter: body assembly,
// input flattening, round-trip re-emission, correlation fallbacks, generation
// passthrough/builder-key rules, hard errors and transport headers. Pure
// data checks — no network, no io_context.
#define BOOST_TEST_MODULE responses_interpreter
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"
#include "dataclass/model_io.hpp"
#include "endpoint/http_request_exception.hpp"
#include "endpoint/model_request.hpp"
#include "llm/responses/interpreter.hpp"

namespace http = boost::beast::http;
using llm::responses::ResponsesInterpreter;
using model_io::AgentInputState;
using model_io::ModelEndpoint;

namespace {

// A Responses-shaped endpoint plus a minimal valid generation. request_path
// is deliberately explicit: the interpreter uses it verbatim.
struct Fixture {
    ModelEndpoint endpoint;
    nlohmann::json generation = nlohmann::json{{"model", "gpt-test"}};

    Fixture() {
        endpoint.base_url = "https://api.example.com";
        endpoint.request_path = "/v1/responses";
        endpoint.auth.scheme = model_io::AuthScheme::None;
    }
};

nlohmann::json body_of(const endpoint::ModelRequestInterpreter::HttpRequest& r) {
    return nlohmann::json::parse(r.body());
}

bool json_array_contains(const nlohmann::json& array, const std::string& value) {
    return std::find(array.begin(), array.end(), nlohmann::json(value)) !=
           array.end();
}

// A minimal ReAct conversation: weather question -> tool call -> tool result
// -> final answer, plus a trailing thank-you turn.
AgentInputState react_conversation() {
    AgentInputState state;

    model_io::UserLoopStep turn;
    turn.user_input.type = model_io::MessageItemType::UserInput;
    turn.user_input.role = "user";
    turn.user_input.content.raw = "What's the weather in London?";

    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    model_io::Content reasoning;
    reasoning.raw = "thinking about the weather";
    step.model_response.reasoning = std::move(reasoning);
    step.model_response.content.raw = "Let me check.";

    model_io::InvokeQuery call;
    call.id = "call_1";
    call.name = "get_weather";
    call.arguments = nlohmann::json{{"city", "London"}};
    step.model_response.invokes = std::vector<model_io::InvokeQuery>{call};

    model_io::MessageItem result;
    result.type = model_io::MessageItemType::InvokeReturn;
    result.role = "tool";
    result.content.raw = "sunny, 21C";
    model_io::InvokeReturn record;
    record.query = call;
    record.output.raw = "sunny, 21C";
    result.invoke_return = std::move(record);
    step.invoke_returns = std::vector<model_io::MessageItem>{result};
    turn.agent_loop_step.push_back(std::move(step));

    model_io::AgentLoopStep final_step;
    final_step.model_response.type = model_io::MessageItemType::ModelResponse;
    final_step.model_response.role = "assistant";
    final_step.model_response.content.raw = "It's sunny in London.";
    turn.agent_loop_step.push_back(std::move(final_step));

    state.turns.push_back(std::move(turn));

    model_io::UserLoopStep thanks;
    thanks.user_input.type = model_io::MessageItemType::UserInput;
    thanks.user_input.role = "user";
    thanks.user_input.content.raw = "thanks!";
    state.turns.push_back(std::move(thanks));
    return state;
}

} // namespace

// ---- request shape -----------------------------------------------------------

BOOST_AUTO_TEST_CASE(empty_state_builds_a_valid_request) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;   // leniency: an empty conversation still builds

    auto request = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK(request.method() == http::verb::post);
    BOOST_CHECK_EQUAL(request.target(), "/v1/responses");
    BOOST_CHECK_EQUAL(std::string(request.at(http::field::host)),
                      "api.example.com");
    BOOST_CHECK_EQUAL(std::string(request.at(http::field::accept)),
                      "text/event-stream");
    BOOST_CHECK_EQUAL(std::string(request.at(http::field::content_type)),
                      "application/json");

    const auto body = body_of(request);
    BOOST_CHECK_EQUAL(body["model"], "gpt-test");
    BOOST_CHECK(body["input"].is_array());
    BOOST_CHECK(body["input"].empty());
    BOOST_CHECK_EQUAL(body["stream"], true);          // SSE-only layer
    BOOST_CHECK_EQUAL(body["store"], false);          // stateless default
    BOOST_CHECK(json_array_contains(body["include"],
                                    "reasoning.encrypted_content"));
    BOOST_CHECK(!body.contains("instructions"));      // empty prompt omits it
    BOOST_CHECK(!body.contains("tools"));             // no tools registered

    const auto length = request.at(http::field::content_length);
    BOOST_REQUIRE(!length.empty());
    BOOST_CHECK_EQUAL(std::stoul(std::string(length)), request.body().size());
}

BOOST_AUTO_TEST_CASE(system_prompt_becomes_instructions) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;
    state.system_prompt.add_section("identity", "Identity", "You are simplex.");

    auto request = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK_NE(body_of(request)["instructions"].get<std::string>().find(
                       "You are simplex."),
                   std::string::npos);
}

BOOST_AUTO_TEST_CASE(tools_become_function_definitions) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;

    model_io::Invocable described;
    described.name = "get_weather";
    described.description = "Look up the weather.";
    const nlohmann::json schema = {{"type", "object"}};
    described.argument_schema = schema;
    state.tools.push_back(described);

    model_io::Invocable bare;
    bare.name = "ping";
    state.tools.push_back(bare);

    const auto tools = body_of(
        interpreter.build_request(state, f.endpoint, f.generation))["tools"];
    BOOST_REQUIRE_EQUAL(tools.size(), 2u);
    BOOST_CHECK_EQUAL(tools[0]["type"], "function");
    BOOST_CHECK_EQUAL(tools[0]["name"], "get_weather");
    BOOST_CHECK_EQUAL(tools[0]["description"], "Look up the weather.");
    BOOST_CHECK_EQUAL(tools[0]["parameters"], schema);
    BOOST_CHECK_EQUAL(tools[1]["name"], "ping");
    BOOST_CHECK(!tools[1].contains("description"));   // empty omitted
}

// ---- input flattening ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(full_react_conversation_flattens_in_order) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state = react_conversation();

    const auto input =
        body_of(interpreter.build_request(state, f.endpoint, f.generation))
            ["input"];
    BOOST_REQUIRE_EQUAL(input.size(), 7u);

    // 0: the user question
    BOOST_CHECK_EQUAL(input[0]["type"], "message");
    BOOST_CHECK_EQUAL(input[0]["role"], "user");
    BOOST_CHECK_EQUAL(input[0]["content"][0]["type"], "input_text");
    BOOST_CHECK_EQUAL(input[0]["content"][0]["text"],
                      "What's the weather in London?");

    // 1: synthesized reasoning from the plain Content
    BOOST_CHECK_EQUAL(input[1]["type"], "reasoning");
    BOOST_CHECK_EQUAL(input[1]["summary"][0]["type"], "summary_text");
    BOOST_CHECK_EQUAL(input[1]["summary"][0]["text"], "thinking about the weather");

    // 2: the assistant's spoken part
    BOOST_CHECK_EQUAL(input[2]["type"], "message");
    BOOST_CHECK_EQUAL(input[2]["role"], "assistant");
    BOOST_CHECK_EQUAL(input[2]["content"][0]["type"], "output_text");
    BOOST_CHECK_EQUAL(input[2]["content"][0]["text"], "Let me check.");

    // 3: the tool call — arguments as a JSON STRING on the wire
    BOOST_CHECK_EQUAL(input[3]["type"], "function_call");
    BOOST_CHECK_EQUAL(input[3]["call_id"], "call_1");
    BOOST_CHECK_EQUAL(input[3]["name"], "get_weather");
    BOOST_CHECK_EQUAL(input[3]["arguments"], "{\"city\":\"London\"}");

    // 4: the correlated tool result
    BOOST_CHECK_EQUAL(input[4]["type"], "function_call_output");
    BOOST_CHECK_EQUAL(input[4]["call_id"], "call_1");
    BOOST_CHECK_EQUAL(input[4]["output"], "sunny, 21C");

    // 5: the final answer, 6: the next user turn
    BOOST_CHECK_EQUAL(input[5]["content"][0]["text"], "It's sunny in London.");
    BOOST_CHECK_EQUAL(input[6]["role"], "user");
    BOOST_CHECK_EQUAL(input[6]["content"][0]["text"], "thanks!");
}

BOOST_AUTO_TEST_CASE(captured_wire_items_reemit_verbatim) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content.raw = "go";

    model_io::AgentLoopStep step;
    step.model_response.role = "assistant";

    // Reasoning: a done item captured by the stream handler.
    const nlohmann::json reasoning_item = {
        {"type", "reasoning"},
        {"id", "rs_1"},
        {"summary", nlohmann::json::array()},
        {"encrypted_content", "ENC-PAYLOAD"},
    };
    model_io::Content reasoning;
    reasoning.raw = "irrelevant once items are captured";
    reasoning.extras = nlohmann::json{{"items", nlohmann::json::array({reasoning_item})}};
    step.model_response.reasoning = std::move(reasoning);

    // Assistant message: a done item with fields synthesis cannot produce.
    const nlohmann::json message_item = {
        {"type", "message"},
        {"id", "msg_9"},
        {"role", "assistant"},
        {"status", "completed"},
        {"content", nlohmann::json::array()},
        {"phase", "commentary"},
    };
    step.model_response.content.raw = "spoken";
    step.model_response.extras = nlohmann::json{
        {"output_items", nlohmann::json::array({reasoning_item, message_item})}};

    // Function call: the captured item carries the provider's own id.
    const nlohmann::json call_item = {
        {"type", "function_call"},
        {"id", "fc_2"},
        {"call_id", "call_2"},
        {"name", "frob"},
        {"arguments", "{}"},
        {"status", "completed"},
    };
    model_io::InvokeQuery call;
    call.id = "call_2";
    call.name = "frob";
    call.arguments = nlohmann::json::object();
    call.extras = call_item;
    step.model_response.invokes = std::vector<model_io::InvokeQuery>{call};

    // Tool result: captured output item with its own id.
    const nlohmann::json output_item = {
        {"type", "function_call_output"},
        {"id", "fco_1"},
        {"call_id", "call_2"},
        {"output", "ok"},
    };
    model_io::MessageItem result;
    result.type = model_io::MessageItemType::InvokeReturn;
    model_io::InvokeReturn record;
    record.query = call;
    record.extras = output_item;
    result.invoke_return = std::move(record);
    step.invoke_returns = std::vector<model_io::MessageItem>{result};

    turn.agent_loop_step.push_back(std::move(step));
    state.turns.push_back(std::move(turn));

    const auto input =
        body_of(interpreter.build_request(state, f.endpoint, f.generation))
            ["input"];
    // 0: user message; then verbatim reasoning, message (only the message
    // entry of output_items), call, output.
    BOOST_REQUIRE_EQUAL(input.size(), 5u);
    BOOST_CHECK_EQUAL(input[1], reasoning_item);
    BOOST_CHECK_EQUAL(input[2], message_item);
    BOOST_CHECK_EQUAL(input[3], call_item);
    BOOST_CHECK_EQUAL(input[4], output_item);
}

BOOST_AUTO_TEST_CASE(call_id_falls_back_positionally_then_omits) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content.raw = "go";

    model_io::AgentLoopStep step;
    step.model_response.role = "assistant";
    model_io::InvokeQuery call;
    call.id = "call_pos";
    call.name = "tool";
    step.model_response.invokes = std::vector<model_io::InvokeQuery>{call};

    // No embedded record: positional alignment with the parent's call wins.
    model_io::MessageItem positional;
    positional.type = model_io::MessageItemType::InvokeReturn;
    positional.content.raw = "positional result";
    // And one result whose parent call id is empty: the key is omitted.
    model_io::MessageItem orphan;
    orphan.type = model_io::MessageItemType::InvokeReturn;
    orphan.content.raw = "orphan result";
    model_io::InvokeQuery empty_id = call;
    empty_id.id.clear();
    step.model_response.invokes =
        std::vector<model_io::InvokeQuery>{call, empty_id};
    step.invoke_returns =
        std::vector<model_io::MessageItem>{positional, orphan};
    turn.agent_loop_step.push_back(std::move(step));
    state.turns.push_back(std::move(turn));

    const auto input =
        body_of(interpreter.build_request(state, f.endpoint, f.generation))
            ["input"];
    // 0 user, 1 call_pos call, 2 call empty-id call, 3 positional output,
    // 4 orphan output.
    BOOST_REQUIRE_EQUAL(input.size(), 5u);
    BOOST_CHECK_EQUAL(input[3]["call_id"], "call_pos");
    BOOST_CHECK_EQUAL(input[4]["output"], "orphan result");
    BOOST_CHECK(!input[4].contains("call_id"));
}

// ---- generation passthrough / builder keys --------------------------------------

BOOST_AUTO_TEST_CASE(generation_passes_through_with_builder_keys_winning) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;   // empty: builder has nothing to say
    f.generation["temperature"] = 0.5;
    f.generation["max_output_tokens"] = 128;
    f.generation["input"] = "caller junk";
    f.generation["instructions"] = "caller instructions";
    f.generation["tools"] = nlohmann::json::array({"web_search"});
    f.generation["stream"] = false;

    const auto body =
        body_of(interpreter.build_request(state, f.endpoint, f.generation));
    BOOST_CHECK_EQUAL(body["temperature"], 0.5);        // verbatim
    BOOST_CHECK_EQUAL(body["max_output_tokens"], 128);  // verbatim
    BOOST_CHECK(body["input"].is_array());              // builder ALWAYS owns
    BOOST_CHECK(body["input"].empty());
    BOOST_CHECK_EQUAL(body["stream"], true);            // SSE-only, forced
    // With no prompt/tools registered the caller's keys pass through —
    // the customization path for provider-native instructions/built-ins.
    BOOST_CHECK_EQUAL(body["instructions"], "caller instructions");
    BOOST_CHECK_EQUAL(body["tools"][0], "web_search");
}

BOOST_AUTO_TEST_CASE(builder_content_overrides_same_named_generation_keys) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;
    state.system_prompt.add_section("id", "Identity", "prompt wins");
    model_io::Invocable tool;
    tool.name = "mine";
    state.tools.push_back(tool);
    f.generation["instructions"] = "caller junk";
    f.generation["tools"] = "caller junk";

    const auto body =
        body_of(interpreter.build_request(state, f.endpoint, f.generation));
    BOOST_CHECK_NE(body["instructions"].get<std::string>().find("prompt wins"),
                   std::string::npos);
    BOOST_REQUIRE_EQUAL(body["tools"].size(), 1u);
    BOOST_CHECK_EQUAL(body["tools"][0]["name"], "mine");
}

BOOST_AUTO_TEST_CASE(store_and_include_interplay) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;

    // store=true (explicit): kept, and no include is injected.
    nlohmann::json generation = f.generation;
    generation["store"] = true;
    auto body = body_of(
        interpreter.build_request(state, f.endpoint, generation));
    BOOST_CHECK_EQUAL(body["store"], true);
    BOOST_CHECK(!body.contains("include"));

    // Caller include list: the reasoning include is appended, not replaced.
    generation = f.generation;
    generation["include"] = nlohmann::json::array({"message.output_text.logprobs"});
    body = body_of(interpreter.build_request(state, f.endpoint, generation));
    BOOST_REQUIRE_EQUAL(body["include"].size(), 2u);
    BOOST_CHECK_EQUAL(body["include"][0], "message.output_text.logprobs");
    BOOST_CHECK(json_array_contains(body["include"],
                                    "reasoning.encrypted_content"));

    // Already present: not duplicated.
    generation["include"] = nlohmann::json::array({"reasoning.encrypted_content"});
    body = body_of(interpreter.build_request(state, f.endpoint, generation));
    BOOST_REQUIRE_EQUAL(body["include"].size(), 1u);
}

BOOST_AUTO_TEST_CASE(string_arguments_pass_through_undouble_encoded) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.content.raw = "go";
    model_io::AgentLoopStep step;
    step.model_response.role = "assistant";
    model_io::InvokeQuery call;
    call.id = "call_s";
    call.name = "tool";
    call.arguments = "{\"x\":1}";   // already the wire string form
    step.model_response.invokes = std::vector<model_io::InvokeQuery>{call};
    turn.agent_loop_step.push_back(std::move(step));
    state.turns.push_back(std::move(turn));

    const auto input =
        body_of(interpreter.build_request(state, f.endpoint, f.generation))
            ["input"];
    // 0: user message; 1: the call (no content -> no synthesized assistant
    // message in between).
    BOOST_REQUIRE_EQUAL(input.size(), 2u);
    BOOST_CHECK_EQUAL(input[1]["type"], "function_call");
    BOOST_CHECK_EQUAL(input[1]["arguments"], "{\"x\":1}");
}

// ---- hard errors ------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(hard_errors_throw_create_request) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;

    const std::array<nlohmann::json, 3> bad_generations = {
        nlohmann::json::object(),
        nlohmann::json{{"model", ""}},
        nlohmann::json{{"model", 42}},
    };
    for (const auto& generation : bad_generations) {
        try {
            interpreter.build_request(state, f.endpoint, generation);
            BOOST_FAIL("expected HttpRequestException for missing model");
        } catch (const HttpRequestException& error) {
            BOOST_CHECK(error.stage()
                        == HttpRequestException::Stage::CreateRequest);
        }
    }

    f.endpoint.base_url.clear();   // hostless: resolver's hard error
    BOOST_CHECK_THROW(
        interpreter.build_request(state, f.endpoint, f.generation),
        HttpRequestException);
}

// ---- transport headers ----------------------------------------------------------------

BOOST_AUTO_TEST_CASE(transport_headers_follow_the_auth_scheme) {
    ResponsesInterpreter interpreter;
    AgentInputState state;

    Fixture f;
    f.endpoint.auth.scheme = model_io::AuthScheme::Bearer;
    f.endpoint.auth.api_key = "sk-secret";
    auto request = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK_EQUAL(std::string(request.at(http::field::authorization)),
                      "Bearer sk-secret");
    BOOST_CHECK_EQUAL(std::string(request.at(http::field::user_agent)),
                      "simplex-cpp");   // the dataclass default

    f.endpoint.auth.scheme = model_io::AuthScheme::CustomHeader;
    f.endpoint.auth.header_name = "x-api-key";
    request = interpreter.build_request(state, f.endpoint, f.generation);
    const auto custom = request.find("x-api-key");
    BOOST_REQUIRE(custom != request.end());
    BOOST_CHECK_EQUAL(std::string(custom->value()), "sk-secret");

    // None — and an empty key under Bearer — never emit a credential.
    f.endpoint.auth.scheme = model_io::AuthScheme::None;
    request = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK(request.count(http::field::authorization) == 0);
    f.endpoint.auth.scheme = model_io::AuthScheme::Bearer;
    f.endpoint.auth.api_key.clear();
    request = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK(request.count(http::field::authorization) == 0);

    // User headers are applied last and win over the standard ones.
    f.endpoint.auth.api_key = "sk-secret";
    f.endpoint.extra_headers["Authorization"] = "Basic dXNlcjpwdw==";
    request = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK_EQUAL(std::string(request.at(http::field::authorization)),
                      "Basic dXNlcjpwdw==");
}

BOOST_AUTO_TEST_CASE(request_path_is_used_verbatim) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;

    auto request = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK_EQUAL(request.target(), "/v1/responses");

    // Prefix in base_url still joins (resolver behavior, pinned here too).
    f.endpoint.base_url = "https://gateway.internal/openai/";
    auto joined = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK_EQUAL(joined.target(), "/openai/v1/responses");
}

// RFC 9110 §7.2: the Host authority carries the port when it is non-default
// for the scheme (443 https / 80 http) — vhost-routing proxies match on it.
BOOST_AUTO_TEST_CASE(host_carries_non_default_port) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;

    // Default ports are omitted for both schemes.
    auto https_default =
        interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK_EQUAL(std::string(https_default.at(http::field::host)),
                      "api.example.com");

    f.endpoint.base_url = "http://localhost";
    auto http_default =
        interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK_EQUAL(std::string(http_default.at(http::field::host)),
                      "localhost");

    // The local-backend shape resolve_endpoint explicitly supports: an
    // explicit port rides along on both schemes.
    f.endpoint.base_url = "http://localhost:11434/v1";
    auto local = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK_EQUAL(std::string(local.at(http::field::host)),
                      "localhost:11434");

    f.endpoint.base_url = "https://gateway.internal:8443";
    auto tls_port = interpreter.build_request(state, f.endpoint, f.generation);
    BOOST_CHECK_EQUAL(std::string(tls_port.at(http::field::host)),
                      "gateway.internal:8443");
}

// The synthesized assistant message must not leak the stream handler's
// refusal key into the output_text part: the API models refusal as its own
// part kind, and a strict backend rejects the schema violation.
BOOST_AUTO_TEST_CASE(synthesized_refusal_becomes_its_own_part) {
    Fixture f;
    ResponsesInterpreter interpreter;
    AgentInputState state;

    // A stream-truncated model_response: output_text accumulated AND a
    // refusal parked in content.extras (exactly what _assemble produces),
    // with no captured output_items — the synthesized path.
    model_io::UserLoopStep turn;
    turn.user_input.type = model_io::MessageItemType::UserInput;
    turn.user_input.role = "user";
    turn.user_input.content.raw = "q";
    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.content.raw = "partial answer";
    step.model_response.content.extras =
        nlohmann::json{{"refusal", "cannot help with that"}};
    turn.agent_loop_step.push_back(std::move(step));
    state.turns.push_back(std::move(turn));

    const auto body =
        body_of(interpreter.build_request(state, f.endpoint, f.generation));

    // The assistant message is the second input item (after the user message).
    const auto& content = body["input"][1]["content"];
    BOOST_REQUIRE_EQUAL(content.size(), 2u);
    BOOST_CHECK_EQUAL(content[0]["type"], "output_text");
    BOOST_CHECK_EQUAL(content[0]["text"], "partial answer");
    BOOST_CHECK(!content[0].contains("refusal"));   // the leak, fixed
    BOOST_CHECK_EQUAL(content[1]["type"], "refusal");
    BOOST_CHECK_EQUAL(content[1]["refusal"], "cannot help with that");
    BOOST_CHECK(!content[1].contains("text"));
}
