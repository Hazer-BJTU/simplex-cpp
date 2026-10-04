#define BOOST_TEST_MODULE qwen_plugin
#include <boost/test/unit_test.hpp>

#include "llm/qwen/dialect.hpp"
#include "endpoint/http_request_exception.hpp"
#include "llm/compat/chat_completions/reader.hpp"
#include "llm/compat/chat_completions/model.hpp"
#include "llm/compat/chat_completions/interpreter.hpp"
#include "llm/compat/chat_completions/events.hpp"
#include "eventbus/event_bus.hpp"
#include "tools/intrinsic/modality_assist/toolset.hpp"
#include "tools/registry.hpp"
#include "fileio/base64.hpp"
#include "fileio/read_prefix.hpp"
#include "loopback_server.hpp"

#include <boost/asio/use_future.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <cstdlib>
#include <filesystem>
#include <thread>

using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

/** Exercise the shipped module, including its factory/build and ABI boundary. */
struct Fixture {
    asio::io_context io;
    llm::LLMDispatcher dispatcher;

    Fixture() {
        const auto override = std::getenv("SIMPLEX_QWEN_TEST_PLUGIN_DIR");
        dispatcher.load_models(fs::path(override ? override : QWEN_PLUGIN_DIR));
        BOOST_REQUIRE(dispatcher.contains("qwen"));
    }

    auto model(Json config = {{"model", "qwen3.8-flash"}}) {
        auto result = dispatcher.create_model("qwen", io.get_executor(), config);
        BOOST_REQUIRE(result);
        return result;
    }
};

Json configuration(unsigned short port, std::string model = "qwen3.8-max") {
    return {
        {"model", std::move(model)},
        {"retry", {{"max_attempts", 0}}},
        {"endpoint", {
            {"base_url", "http://127.0.0.1:" + std::to_string(port)},
            {"auth", {{"api_key", "fixture-key"}}}
        }}
    };
}

model_io::AgentInputState input() {
    model_io::AgentInputState state;
    state.system_prompt.add_section("system", "", "Answer the question.");
    auto& user = state.turns.emplace_back().user_input;
    user.role = "user";
    user.content.emplace_back().raw = "Describe the scene.";
    return state;
}

Json frame(Json delta, Json finish = nullptr) {
    return {
        {"id", "qwen-fixture"}, {"model", "qwen3.8-max"},
        {"choices", Json::array({{
            {"index", 0}, {"delta", std::move(delta)},
            {"finish_reason", std::move(finish)}
        }})}
    };
}

std::string sse(const Json& value) { return "data: " + value.dump() + "\n\n"; }
std::string answer() {
    return sse(frame({{"content", "A red square."}}, "stop")) + "data: [DONE]\n\n";
}

/** Capture wire requests and answer on the server thread; inspect only after join. */
loopback::Serve respond(std::vector<http::request<http::string_body>>& requests,
                       std::string body, http::status status = http::status::ok) {
    return [&requests, body = std::move(body), status](tcp::socket& socket) {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request);
        requests.push_back(std::move(request));
        http::response<http::string_body> response(status, 11);
        response.set(http::field::content_type, "text/event-stream");
        response.set(http::field::connection, "close");
        response.body() = body;
        response.prepare_payload();
        http::write(socket, response);
    };
}

Json wire(const model_io::AgentInputState& state, Json generation) {
    llm::chat_completions::ChatCompletionsInterpreter interpreter(llm::qwen::qwen_dialect());
    return Json::parse(interpreter.build_request(
        state, llm::qwen::qwen_dialect()->default_endpoint(), generation).body());
}

} // namespace

BOOST_FIXTURE_TEST_CASE(options_defaults_and_atomic_updates, Fixture) {
    auto instance = model();
    const llm::LLMModel& view = *instance;
    const auto descriptors = view.get_options();
    BOOST_TEST(descriptors.size() == 3u);
    BOOST_TEST(descriptors[1]["options"] == Json::array({"enabled", "disabled"}));
    BOOST_TEST(view.get_current_options() == Json({
        {"model", "qwen3.8-flash"}, {"enable_thinking", "enabled"}, {"reasoning_effort", "xhigh"}
    }));
    instance->handle_options({{"model", "qwen3.8-max"}, {"enable_thinking", "disabled"},
                              {"reasoning_effort", "low"}});
    BOOST_TEST(instance->get_current_options().at("enable_thinking") == "disabled");
    const auto before = instance->generation();
    for (const auto& invalid : std::vector<Json>{
        nullptr, Json::array(), {{"enable_thinking", "false"}}, {{"enable_thinking", 1}}, {{"enable_thinking", false}},
        {{"model", "other"}}, {{"model", "qwen3.8-flash"}, {"reasoning_effort", "high"}},
        {{"temperature", 0.1}}, {{"reasoning_effort", nullptr}}
    }) {
        BOOST_CHECK_THROW(instance->handle_options(invalid), std::invalid_argument);
        BOOST_TEST(instance->generation() == before);
    }
    instance->handle_options(Json::object());
    BOOST_TEST(view.get_options() == descriptors);
    BOOST_TEST(instance->generation() == before);
}

BOOST_FIXTURE_TEST_CASE(budget_precedence_aliases_and_configuration_validation, Fixture) {
    auto instance = model({{"model", "qwen3.8-flash"}, {"thinking_budget", 8000}});
    BOOST_TEST(instance->get_current_options().at("reasoning_effort") == "medium");
    instance->handle_options({{"reasoning_effort", "low"}});
    BOOST_TEST(!instance->generation().contains("thinking_budget"));
    BOOST_TEST(instance->get_current_options().at("reasoning_effort") == "low");
    const auto before = instance->generation();
    BOOST_CHECK_THROW(instance->set_generation(Json{{"thinking_budget", 100}}), std::invalid_argument);
    BOOST_CHECK_THROW(instance->set_generation(Json{{"endpoint", Json::object()}}), std::invalid_argument);
    BOOST_TEST(instance->generation() == before);
    auto alias = model({{"model", "qwen3.8-max"}, {"reasoning", {{"effort", "high"}}}});
    BOOST_TEST(alias->get_current_options().at("reasoning_effort") == "xhigh");
    alias->set_generation(llm::GenerationPreset{.effort = llm::ReasoningEffort::Low});
    BOOST_TEST(alias->get_current_options().at("reasoning_effort") == "low");
    alias->set_generation(Json{{"reasoning_effort", "medium"}});
    BOOST_TEST(alias->get_current_options().at("reasoning_effort") == "medium");
    for (const auto& patch : std::vector<Json>{
        {{"enable_thinking", "yes"}}, {{"thinking_budget", -1}}, {{"thinking_budget", 262145}},
        {{"thinking_budget", 1.5}}, {{"preserve_thinking", 1}}, {{"tool_stream", "true"}},
        {{"reasoning_effort", "low"}, {"thinking_budget", 0}},
        {{"reasoning", {{"effort", "low"}}}, {"thinking_budget", 0}},
        {{"reasoning_effort", "invalid"}}, {{"extra_body", Json::object()}},
        {{"thinking", {{"type", "enabled"}}}}, {{"modalities", {"audio"}}},
        {{"tool_choice", "required"}}
    }) {
        Json config = {{"model", "qwen3.8-flash"}};
        config.update(patch);
        BOOST_CHECK(!dispatcher.create_model("qwen", io.get_executor(), config));
    }
    // Untested models are configurable without pretending their defaults are known.
    auto other = model({{"model", "custom-vision"}});
    BOOST_TEST(other->get_current_options() == Json({{"model", "custom-vision"}}));
    auto metadata = asio::co_spawn(io, other->provider_info(), asio::use_future);
    io.run();
    BOOST_CHECK_THROW(metadata.get(), llm::LLMUnsupportedOperation);
}

BOOST_AUTO_TEST_CASE(dialect_replays_reasoning_only_when_requested_and_preserves_images) {
    auto state = input();
    auto& user = state.turns[0].user_input;
    for (const auto& url : {"https://images.example/image.png", "data:image/jpeg;base64,distinctive-image"}) {
        auto& image = user.content.emplace_back();
        image.modality = model_io::Modality::Image;
        image.type = model_io::ContentType::ExternalRef;
        image.raw = url;
    }
    auto& step = state.turns[0].agent_loop_step.emplace_back();
    step.model_response.role = "assistant";
    step.model_response.reasoning.emplace().raw = "prior reasoning";
    step.model_response.content.emplace_back().raw = "prior answer";
    for (const auto& name : {"qwen3.8-flash", "qwen3.8-max", "qwen3.8-max-0902"}) {
        auto body = wire(state, {{"model", name}});
        BOOST_TEST(body["messages"][1]["content"][1]["image_url"]["url"] == user.content[1].raw);
        BOOST_TEST(body["messages"][1]["content"][2]["image_url"]["url"] == user.content[2].raw);
        BOOST_TEST(body["messages"][2].contains("reasoning_content") == (std::string(name) != "qwen3.8-flash"));
        BOOST_TEST(!body.contains("n"));
        BOOST_TEST(!body.contains("thinking"));
        BOOST_TEST(body["stream_options"]["include_usage"] == true);
    }
    BOOST_TEST(wire(state, {{"model", "qwen3.8-flash"}, {"preserve_thinking", true}})
        ["messages"][2]["reasoning_content"] == "prior reasoning");
    BOOST_TEST(!wire(state, {{"model", "qwen3.8-max"}, {"preserve_thinking", false}})
        ["messages"][2].contains("reasoning_content"));
    state.turns[0].user_input.content.back().modality = model_io::Modality::Audio;
    BOOST_CHECK_THROW(wire(state, {{"model", "qwen3.8-flash"}}), HttpRequestException);
}

BOOST_FIXTURE_TEST_CASE(streamed_tool_round_trip_usage_and_endpoint_override, Fixture) {
    const auto calls = [](Json call) { return Json{{"tool_calls", Json::array({std::move(call)})}}; };
    const auto stream = sse(frame({{"reasoning_content", "look "}}))
        + sse(frame({{"reasoning_content", "closely"}}))
        + sse(frame(calls({{"index", 3}, {"id", "call-1"}, {"type", "function"},
                          {"function", {{"name", "inspect"}, {"arguments", "{\"value\":"}}}})))
        + sse(frame(calls({{"index", 3}, {"function", {{"arguments", "1}"}}}})))
        + sse(frame(Json::object(), "tool_calls"))
        + sse({{"choices", Json::array()}, {"usage", {
            {"prompt_tokens", 20}, {"completion_tokens", 6}, {"total_tokens", 26},
            {"prompt_tokens_details", {{"cached_tokens", 12}}},
            {"completion_tokens_details", {{"reasoning_tokens", 4}}}
        }}}) + "data: [DONE]\n\n";
    std::vector<http::request<http::string_body>> requests;
    loopback::SequenceServer server({respond(requests, stream), respond(requests, answer())});
    auto config = configuration(server.wait_listening());
    config["endpoint"]["base_url"] = config["endpoint"]["base_url"].get<std::string>() + "/compatible-mode/v1";
    config["endpoint"]["request_path"] = "/chat/completions";
    auto instance = model(config);
    auto state = input();
    state.tools.push_back({.name = "inspect", .description = "Inspect",
        .argument_schema = {{"type", "object"}, {"properties", {{"value", {{"type", "integer"}}}}}}});
    std::vector<std::string> reasoning;
    auto subscription = eventbus::default_bus().subscribe<llm::chat_completions::ReasoningDeltaEvent>(
        [&](const auto& event) {
            BOOST_TEST(event.provider == "qwen");
            reasoning.push_back(event.reasoning);
        });
    auto first = asio::co_spawn(io, instance->converse(state), asio::use_future);
    io.run();
    auto response = first.get();
    BOOST_REQUIRE(response.reasoning);
    BOOST_TEST(response.reasoning->raw == "look closely");
    BOOST_REQUIRE(response.invokes);
    BOOST_TEST(response.invokes->at(0).arguments == Json({{"value", 1}}));
    BOOST_REQUIRE(response.cost);
    BOOST_TEST(response.cost->prompt == 20u);
    BOOST_TEST(response.cost->generated == 6u);
    BOOST_TEST(response.cost->cache_hit == 12u);
    BOOST_TEST(reasoning.size() == 2u);
    auto& step = state.turns[0].agent_loop_step.emplace_back();
    step.model_response = response;
    model_io::MessageItem result;
    result.type = model_io::MessageItemType::InvokeReturn;
    result.content.emplace_back().raw = "inspected";
    result.invoke_return.emplace().query = response.invokes->at(0);
    result.invoke_return->output = result.content[0];
    step.invoke_returns = std::vector{result};
    io.restart();
    auto second = asio::co_spawn(io, instance->converse(state), asio::use_future);
    io.run();
    auto final = second.get();
    server.join();
    BOOST_REQUIRE(requests.size() == 2u);
    BOOST_TEST(std::string(requests[0].target()) == "/compatible-mode/v1/chat/completions");
    BOOST_TEST(std::string(requests[0][http::field::authorization]) == "Bearer fixture-key");
    BOOST_TEST(std::string(requests[0][http::field::user_agent]) == "simplex-cpp/qwen");
    auto body = Json::parse(requests[1].body());
    BOOST_TEST(body["messages"][2]["reasoning_content"] == "look closely");
    BOOST_TEST(body["messages"][3]["tool_call_id"] == "call-1");
    BOOST_TEST(body["messages"][3]["content"] == "inspected");
    BOOST_TEST(final.content[0].raw == "A red square.");
    BOOST_TEST(!final.cost);
}

BOOST_FIXTURE_TEST_CASE(auxiliary_tool_sends_images_in_an_isolated_exchange, Fixture) {
    std::vector<http::request<http::string_body>> requests;
    loopback::SequenceServer server({respond(requests, answer())});
    auto instance = model(configuration(server.wait_listening(), "qwen3.8-flash"));
    tools::ToolRegistry registry;
    registry.add(std::make_shared<tools::intrinsic::ModalityAssistToolSet>(instance));
    model_io::InvokeQuery query;
    query.id = "vision";
    query.name = "modality_assist";
    query.arguments = {{"request", "Describe"}, {"path", {QWEN_IMAGE_FIXTURE, QWEN_JPEG_FIXTURE, QWEN_IMAGE_FIXTURE}}};
    auto task = asio::co_spawn(io, registry.execute({query}, io.get_executor()), asio::use_future);
    io.run();
    auto result = task.get().at(0);
    server.join();
    BOOST_REQUIRE(!tools::is_error(result));
    BOOST_REQUIRE(requests.size() == 1u);
    auto body = Json::parse(requests[0].body());
    BOOST_TEST(body["messages"].size() == 2u);
    BOOST_TEST(!body.contains("tools"));
    const auto& parts = body["messages"][1]["content"];
    BOOST_REQUIRE(parts.size() == 4u);
    const auto url = parts[1]["image_url"]["url"].get<std::string>();
    BOOST_TEST(url.starts_with("data:image/png;base64,"));
    BOOST_TEST(fileio::base64_decode(url.substr(url.find(',') + 1))
        == fileio::read_prefix(QWEN_IMAGE_FIXTURE, 1024));
    const auto jpeg = parts[2]["image_url"]["url"].get<std::string>();
    BOOST_TEST(jpeg.starts_with("data:image/jpeg;base64,"));
    BOOST_TEST(fileio::base64_decode(jpeg.substr(jpeg.find(',') + 1))
        == fileio::read_prefix(QWEN_JPEG_FIXTURE, 1024));
    BOOST_TEST(parts[3] == parts[1]);
    BOOST_TEST(result.output.raw.find("A red square.") != std::string::npos);
    BOOST_TEST(result.output.raw.find("base64,") == std::string::npos);
    // A later invalid file fails before converse; the closed endpoint is never contacted.
    query.arguments["path"] = {QWEN_IMAGE_FIXTURE, "/nonexistent/qwen-test-image.png"};
    io.restart();
    auto invalid = asio::co_spawn(io, registry.execute({query}, io.get_executor()), asio::use_future);
    io.run();
    const auto failure = invalid.get().at(0);
    BOOST_REQUIRE(tools::is_error(failure));
    BOOST_TEST(failure.output.raw.find("path[1]") != std::string::npos);
    BOOST_TEST(failure.output.raw.find("base64,") == std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(option_updates_remain_coherent_across_threads, Fixture) {
    auto instance = model();
    std::atomic<bool> valid{true};
    const auto update = [&](const char* name, const char* effort) {
        try {
            for (int i = 0; i < 100; ++i) {
                instance->handle_options({{"model", name}, {"reasoning_effort", effort}});
                const auto snapshot = instance->get_current_options();
                const auto model_name = snapshot.at("model");
                const auto level = snapshot.at("reasoning_effort");
                if (!((model_name == "qwen3.8-flash" && level == "low")
                    || (model_name == "qwen3.8-max" && level == "medium"))) {
                    valid = false;
                }
            }
        } catch (...) {
            valid = false;
        }
    };
    std::thread first(update, "qwen3.8-flash", "low");
    std::thread second(update, "qwen3.8-max", "medium");
    first.join();
    second.join();
    BOOST_TEST(valid.load());
    auto independent = model();
    BOOST_TEST(independent->get_current_options().at("reasoning_effort") == "xhigh");
}

BOOST_FIXTURE_TEST_CASE(failure_paths_preserve_diagnostics_and_do_not_retry_assembly, Fixture) {
    const auto malformed = sse(frame({{"tool_calls", Json::array({
        {{"index", 0}, {"id", "distinctive-duplicate"}, {"type", "function"},
         {"function", {{"name", "inspect"}, {"arguments", "{\"private\":\"distinctive-arguments\"}"}}}},
        {{"index", 1}, {"id", "distinctive-duplicate"}, {"type", "function"},
         {"function", {{"name", "inspect"}, {"arguments", "{}"}}}}
    })}}, "tool_calls")) + "data: [DONE]\n\n";
    const auto api_error = sse({{"error", {{"code", "InvalidParameter"},
        {"message", "scripted provider rejection"}}}});
    for (const auto& stream : {malformed, api_error}) {
        std::vector<http::request<http::string_body>> requests;
        loopback::SequenceServer server({respond(requests, stream)});
        auto config = configuration(server.wait_listening());
        config["retry"] = {{"max_attempts", 2}, {"initial_backoff_ms", 1}, {"max_backoff_ms", 1}};
        auto instance = model(config);
        io.restart();
        auto result = asio::co_spawn(io, instance->converse(input()), asio::use_future);
        io.run();
        server.join();
        if (stream == malformed) {
            BOOST_CHECK_EXCEPTION(result.get(), llm::chat_completions::ChatCompletionsAssemblyException,
                [](const auto& error) {
                    const std::string message = error.what();
                    return message.find("duplicate") != std::string::npos
                        && message.find("distinctive-duplicate") == std::string::npos
                        && message.find("distinctive-arguments") == std::string::npos;
                });
        } else {
            BOOST_CHECK_EXCEPTION(result.get(), llm::chat_completions::ChatCompletionsApiException,
                [](const auto& error) {
                    return error.details().at("code") == "InvalidParameter"
                        && std::string(error.what()) == "scripted provider rejection";
                });
        }
        BOOST_TEST(requests.size() == 1u);
    }
}

BOOST_FIXTURE_TEST_CASE(transient_http_failure_retries_and_exhaustion_propagates, Fixture) {
    for (bool recover : {true, false}) {
        std::vector<http::request<http::string_body>> requests;
        loopback::SequenceServer server({
            respond(requests, "busy", http::status::service_unavailable),
            respond(requests, recover ? answer() : "busy",
                recover ? http::status::ok : http::status::service_unavailable)
        });
        auto config = configuration(server.wait_listening());
        config["retry"] = {{"max_attempts", 1}, {"initial_backoff_ms", 1}, {"max_backoff_ms", 1}};
        auto instance = model(config);
        io.restart();
        auto result = asio::co_spawn(io, instance->converse(input()), asio::use_future);
        io.run();
        server.join();
        if (recover) {
            BOOST_TEST(result.get().content[0].raw == "A red square.");
        } else {
            BOOST_CHECK_THROW(result.get(), HttpRequestException);
        }
        BOOST_REQUIRE(requests.size() == 2u);
        BOOST_TEST(requests[0].body() == requests[1].body());
    }
}

BOOST_FIXTURE_TEST_CASE(cancellation_joins_a_suspended_exchange, Fixture) {
    asio::cancellation_signal cancellation;
    std::atomic<bool> disconnected{false};
    loopback::SequenceServer server({[&](tcp::socket& socket) {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        http::read(socket, buffer, request);
        // Cancel only after the request arrived: no timer/timing assumption.
        asio::post(io, [&] { cancellation.emit(asio::cancellation_type::terminal); });
        char byte;
        boost::system::error_code error;
        socket.read_some(asio::buffer(&byte, 1), error);
        disconnected = bool(error);
    }});
    auto instance = model(configuration(server.wait_listening()));
    auto state = input();
    const Json before = state;
    auto result = asio::co_spawn(io, instance->converse(state),
        asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
    io.run();
    server.join();
    BOOST_CHECK_EXCEPTION(result.get(), boost::system::system_error,
        [](const auto& error) { return error.code() == asio::error::operation_aborted; });
    BOOST_TEST(disconnected.load());
    BOOST_TEST(Json(state) == before);
}
