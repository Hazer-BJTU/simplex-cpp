#define BOOST_TEST_MODULE custom_chat
#include <boost/test/unit_test.hpp>
#include <boost/asio.hpp>
#include <filesystem>
#include <atomic>
#include "llm/custom_chat/model.hpp"
#include "eventbus/event_bus.hpp"
#include "llm/compat/chat_completions/events.hpp"
#include "llm/compat/chat_completions/interpreter.hpp"
#include "llm/compat/chat_completions/reader.hpp"
#include "loopback_server.hpp"

using namespace llm::custom_chat;
using namespace llm::chat_completions;

namespace {
Json configuration() {
    return Json::parse(R"({
      "replay_assistant_reasoning": true,
      "request": {
        "message_patches": [{"role":"assistant", "patch":[
          {"op":"move", "from":"/reasoning_content", "path":"/thought", "if_missing":"skip"}]}],
        "patch": [
          {"op":"remove", "path":"/n"},
          {"op":"remove", "path":"/stream_options/include_usage"},
          {"op":"set", "path":"/thinking/type", "value":"base"}]
      },
      "response":{"chunk_patch":[
        {"op":"move", "from":"/choices/0/delta/thought", "path":"/choices/0/delta/reasoning_content", "if_missing":"skip"},
        {"op":"set", "path":"/usage", "value":{"prompt_tokens":999,"completion_tokens":999}}]},
      "token_cost":{"prompt":"/in", "generated":"/out", "cache_hit":"/cached"},
      "models":[{"id":"declared","description":"local"}],
      "options":[
        {"name":"thinking", "default":"on", "choices":[
          {"value":"on", "patch":[{"op":"set","path":"/thinking/type","value":"enabled"},
                                    {"op":"set","path":"/only_on","value":true}]},
          {"value":"off", "patch":[{"op":"set","path":"/thinking/type","value":"disabled"}]}]},
        {"name":"model", "choices":[
          {"value":"alternate", "patch":[{"op":"set","path":"/model","value":"alternate"}]}]}
      ]
    })");
}
Json startup(unsigned short port = 1) {
    return {{"model", "unlisted"}, {"compatibility", configuration()},
            {"reasoning", {{"effort", "untouched"}}}, {"n", 9},
            {"stream_options", {{"include_usage", false}, {"custom", true}}},
            {"unknown", Json::array({nullptr, "native", 4})},
            {"retry", {{"max_attempts", 0}}},
            {"endpoint", {{"base_url", "http://127.0.0.1:" + std::to_string(port)},
                          {"auth", {{"scheme", "none"}}}}}};
}
model_io::AgentInputState history() {
    model_io::AgentInputState state;
    model_io::UserLoopStep turn;
    turn.user_input.role = "user";
    model_io::Content text;
    text.type = model_io::ContentType::Text;
    text.raw = "hello";
    turn.user_input.content.push_back(text);
    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.content.push_back(text);
    text.raw = "old thought";
    step.model_response.reasoning = text;
    turn.agent_loop_step.push_back(step);
    state.turns.push_back(turn);
    return state;
}
std::string sse(const Json& value) { return "data: " + value.dump() + "\n\n"; }
std::string stream() {
    return sse(Json::parse(R"({"choices":[{"index":0,"delta":{"thought":"thinking","content":"answer"},"finish_reason":"stop"}],"usage":{"in":12,"out":3,"cached":7}})"))
        + "data: [DONE]\n\n";
}
loopback::Serve capture(Json& captured, std::string wire = stream()) {
    return [&captured, wire = std::move(wire)](tcp::socket& socket) {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request);
        captured = Json::parse(request.body());
        http::response<http::string_body> response(http::status::ok, 11);
        response.set(http::field::content_type, "text/event-stream");
        response.body() = wire;
        response.prepare_payload();
        http::write(socket, response);
    };
}
model_io::MessageItem exchange(asio::io_context& io, llm::LLMModel& model) {
    std::optional<model_io::MessageItem> result;
    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        result = co_await model.converse(history());
    }, [&](std::exception_ptr error) { failure = error; });
    io.run(); io.restart();
    if (failure) std::rethrow_exception(failure);
    return *result;
}
model_io::MessageItem read(asio::io_context& io, std::string wire, const Json& config) {
    auto dialect = std::make_shared<ConfigurableDialect>(std::make_shared<CompatibilityConfig>(config));
    ChatCompletionsReader reader(io.get_executor(), dialect);
    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        while (co_await reader.next()) {}
    }, [&](std::exception_ptr error) { failure = error; });
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        co_await reader.handler()->put(wire);
    }, asio::detached);
    io.run(); io.restart();
    if (failure) std::rethrow_exception(failure);
    return reader.response();
}
}

BOOST_AUTO_TEST_CASE(plugin_wire_options_and_raw_cost) {
    Json first, second;
    loopback::SequenceServer server({capture(first), capture(second)});
    const auto config = startup(server.wait_listening());
    llm::LLMDispatcher dispatcher;
    BOOST_REQUIRE(dispatcher.load_models(std::filesystem::path(CUSTOM_CHAT_PLUGIN_DIR)) > 0);
    asio::io_context io;
    auto model = dispatcher.create_model("chat-completions", io.get_executor(), config);
    BOOST_REQUIRE(model);
    std::vector<std::string> observed_models;
    eventbus::EventBus::ScopedSubscription subscription =
        eventbus::default_bus().subscribe<ReasoningDeltaEvent>(
        [&](const ReasoningDeltaEvent& event) { observed_models.push_back(event.model); });
    BOOST_TEST(model->get_options().size() == 2u);
    BOOST_TEST(model->get_current_options()["thinking"] == "on");
    const auto result = exchange(io, *model);
    model->handle_options({{"thinking", "off"}, {"model", "alternate"}});
    BOOST_CHECK_THROW(model->handle_options(Json{{"thinking", "on"}, {"zzz", "bad"}}), std::invalid_argument);
    BOOST_TEST(model->get_current_options()["thinking"] == "off");
    model->set_generation(Json{{"temperature", 0.2}});
    exchange(io, *model);
    server.join();
    BOOST_TEST(!first.contains("compatibility"));
    BOOST_TEST(!first.contains("n"));
    BOOST_TEST(!first["stream_options"].contains("include_usage"));
    BOOST_TEST(first["stream_options"]["custom"] == true);
    BOOST_TEST(first["reasoning"] == config["reasoning"]);
    BOOST_TEST(first["unknown"] == config["unknown"]);
    BOOST_TEST(first["thinking"]["type"] == "enabled");
    BOOST_TEST(first["messages"][1]["thought"] == "old thought");
    BOOST_TEST(!first["messages"][1].contains("reasoning_content"));
    BOOST_TEST(second["thinking"]["type"] == "disabled");
    BOOST_TEST(!second.contains("only_on"));
    BOOST_TEST(second["model"] == "alternate");
    BOOST_REQUIRE_EQUAL(observed_models.size(), 2u);
    BOOST_TEST(observed_models[0] == "unlisted");
    BOOST_TEST(observed_models[1] == "alternate");
    BOOST_REQUIRE(result.reasoning);
    BOOST_TEST(result.reasoning->raw == "thinking");
    BOOST_REQUIRE(result.cost);
    BOOST_TEST(result.cost->prompt == 12u);
    BOOST_TEST(result.cost->generated == 3u);
    BOOST_TEST(result.cost->cache_hit == 7u);
    BOOST_TEST(result.extras->at("usage") == Json::parse(R"({"in":12,"out":3,"cached":7})"));
}

BOOST_AUTO_TEST_CASE(native_parameters_and_defaults) {
    auto dialect = std::make_shared<ConfigurableDialect>(std::make_shared<CompatibilityConfig>());
    ChatCompletionsInterpreter interpreter(dialect);
    model_io::ModelEndpoint endpoint;
    endpoint.base_url = "http://localhost";
    const Json generation = {{"model", "m"}, {"stream", false}, {"n", 8},
        {"stream_options", nullptr}, {"reasoning", {{"effort", "custom"}}},
        {"extra_body", {{"x", 1}}}};
    auto body = Json::parse(interpreter.build_request(history(), endpoint, generation).body());
    for (const auto& [key, value] : generation.items()) BOOST_TEST(body[key] == value);
    body = Json::parse(interpreter.build_request({}, endpoint, {{"model", "m"}}).body());
    BOOST_TEST(body["stream"] == true);
    BOOST_TEST(body["n"] == 1);
    BOOST_TEST(body["stream_options"]["include_usage"] == true);
}

BOOST_AUTO_TEST_CASE(patch_paths_and_failure_locations) {
    Json doc = Json::parse(R"({"arr":[{"a":2}],"a/b":{"~":3}})");
    PatchProgram(Json::parse(R"([
        {"op":"copy","from":"/a~1b/~0","path":"/nested/value"},
        {"op":"move","from":"/arr/0/a","path":"/arr/0/b"},
        {"op":"move","from":"/arr/0","path":"/arr/0"},
        {"op":"remove","path":"/absent/child"},
        {"op":"copy","from":"/arr/9","path":"/skip","if_missing":"skip"}
    ])"), "rules").apply(doc);
    BOOST_TEST(doc["nested"]["value"] == 3);
    BOOST_TEST(doc["arr"][0]["b"] == 2);
    BOOST_TEST(!doc["arr"][0].contains("a"));
    BOOST_TEST(!doc.contains("skip"));
    for (const auto& rules : {
        R"([{"op":"set","path":"/arr/1","value":0}])",
        R"([{"op":"set","path":"/nested/value/x","value":0}])",
        R"([{"op":"copy","from":"/missing","path":"/x"}])"}) {
        try { PatchProgram(Json::parse(rules), "test/rules").apply(doc); BOOST_FAIL("expected failure"); }
        catch (const std::invalid_argument& error) {
            BOOST_TEST(std::string(error.what()).find("test/rules/0") != std::string::npos);
        }
    }
    for (const auto& rules : {
        R"([{"op":"set","path":"/bad~2","value":0}])",
        R"([{"op":"move","from":"/a","path":"/a/b"}])",
        R"([{"op":"remove","path":""}])",
        R"([{"op":"unknown","path":"/a"}])"}) {
        BOOST_CHECK_THROW(PatchProgram(Json::parse(rules), "invalid"), std::invalid_argument);
    }
}

BOOST_AUTO_TEST_CASE(usage_last_block_no_coercion_and_null_mapping) {
    asio::io_context io;
    const Json final = Json::parse(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})");
    const auto finish = sse(final) + "data: [DONE]\n\n";
    auto result = read(io, sse({{"usage", {{"prompt_tokens", 10}, {"completion_tokens", 4}}}})
                          + finish, Json::object());
    BOOST_REQUIRE(result.cost);
    BOOST_TEST(result.cost->prompt == 10u);
    BOOST_TEST(result.cost->cache_hit == 0u);
    for (const auto& bad : {Json(-1), Json(1.5), Json("3"), Json(nullptr)}) {
        result = read(io, sse({{"usage", {{"prompt_tokens", 10}, {"completion_tokens", bad}}}})
                         + finish, Json::object());
        BOOST_TEST(!result.cost);
    }
    result = read(io, sse({{"usage", {{"prompt_tokens", 10}, {"completion_tokens", 4}}}})
                     + sse({{"usage", {{"prompt_tokens", 11}}}}) + finish, Json::object());
    BOOST_TEST(!result.cost); // Never combine different snapshots.
    result = read(io, sse({{"usage", {{"prompt_tokens", 10}, {"completion_tokens", 4}}}})
                     + finish, {{"token_cost", {{"prompt", nullptr}}}});
    BOOST_TEST(!result.cost);
}

BOOST_AUTO_TEST_CASE(response_rule_errors_propagate) {
    asio::io_context io;
    Json config = {{"response", {{"chunk_patch", Json::array({
        {{"op", "copy"}, {"from", "/missing"}, {"path", "/x"}}
    })}}}};
    BOOST_CHECK_THROW(read(io, stream(), config), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(local_catalog_and_instance_isolation) {
    asio::io_context io;
    CustomChatCompletionsModel a(io.get_executor(), startup()), b(io.get_executor(), startup());
    BOOST_REQUIRE(a.build()); BOOST_REQUIRE(b.build());
    a.handle_options({{"thinking", "off"}});
    BOOST_TEST(b.get_current_options()["thinking"] == "on");
    BOOST_CHECK_THROW(a.set_generation(Json{{"compatibility", Json::object()}}), std::invalid_argument);
    Json catalog;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> { catalog = co_await a.provider_info(); }, asio::detached);
    io.run();
    BOOST_TEST(catalog == configuration()["models"]); // Endpoint port 1 is never contacted.
    auto bad = configuration(); bad["options"][0]["default"] = "missing";
    BOOST_CHECK_THROW(CompatibilityConfig{bad}, std::invalid_argument);
    bad = configuration(); bad["request"]["typo"] = true;
    BOOST_CHECK_THROW(CompatibilityConfig{bad}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(retry_keeps_snapshot_and_next_exchange_uses_new_options) {
    asio::io_context io;
    std::unique_ptr<CustomChatCompletionsModel> model;
    std::atomic<CustomChatCompletionsModel*> published{nullptr};
    Json first, retry, next;
    loopback::SequenceServer server({
        [&](tcp::socket& socket) {
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request);
            first = Json::parse(request.body());
            // This callback runs on the server thread during the first exchange.
            auto* active = published.load();
            active->handle_options({{"thinking", "off"}, {"model", "alternate"}});
            active->set_generation(Json{{"temperature", 0.9}});
            http::response<http::string_body> response(http::status::service_unavailable, 11);
            response.body() = "retry";
            response.prepare_payload();
            http::write(socket, response);
        }, capture(retry), capture(next)});
    auto config = startup(server.wait_listening());
    config["retry"] = {{"max_attempts", 1}, {"initial_backoff_ms", 1}, {"max_backoff_ms", 1}};
    model = std::make_unique<CustomChatCompletionsModel>(io.get_executor(), config);
    BOOST_REQUIRE(model->build());
    published.store(model.get());
    exchange(io, *model);
    exchange(io, *model);
    server.join();
    BOOST_TEST(first == retry);
    BOOST_TEST(first["thinking"]["type"] == "enabled");
    BOOST_TEST(next["thinking"]["type"] == "disabled");
    BOOST_TEST(next["model"] == "alternate");
    BOOST_TEST(next["temperature"] == 0.9);
    BOOST_TEST(!next.contains("only_on"));
}

BOOST_AUTO_TEST_CASE(option_declaration_order_and_response_failure_over_http) {
    Json config = configuration();
    config["options"].push_back(Json::parse(R"({"name":"last","default":"selected","choices":[
      {"value":"selected","patch":[{"op":"set","path":"/thinking/type","value":"last"}]}]})"));
    auto declaration = std::make_shared<CompatibilityConfig>(config);
    ConfigurableDialect dialect(declaration, declaration->defaults);
    Json body = {{"messages", Json::array()}};
    dialect.transform_request(body);
    BOOST_TEST(body["thinking"]["type"] == "last");

    Json captured;
    loopback::SequenceServer server({capture(captured)});
    auto startup_config = startup(server.wait_listening());
    startup_config["compatibility"]["response"]["chunk_patch"] = Json::parse(
        R"([{"op":"copy","from":"/absent","path":"/x"}])");
    asio::io_context io;
    CustomChatCompletionsModel model(io.get_executor(), startup_config);
    BOOST_REQUIRE(model.build());
    BOOST_CHECK_THROW(exchange(io, model), std::exception);
    server.join();
}
