/**
 * @file test_glm_model.cpp
 * @brief Full-flow converse() tests for the glm provider over a loopback HTTP
 *        server.
 *
 * The exchange runs against a scripted SSE stream on 127.0.0.1: the server
 * captures the request body, so every dialect policy is asserted on the wire
 * (effort translation, request hygiene, reasoning replay), and the assembled
 * MessageItem is asserted on the way back (reasoning slot, parsed invokes,
 * cache-hit cost accounting, the finish_reason mapping). The error-chunk path
 * covers the chat layer's ApiException surfacing, and the build gate covers
 * the configuration the service would reject.
 */

#define BOOST_TEST_MODULE glm_model
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "eventbus/event_bus.hpp"
#include "llm/compat/chat_completions/events.hpp"
#include "llm/compat/chat_completions/model.hpp"
#include "llm/glm/dialect.hpp"
#include "loopback_server.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace {

// ChatCompletionsModel's constructor is protected: instances are minted by
// the plugin factory. The test mints its own with the real dialect, and its
// build() mirrors the plugin's startup gate (validate the configuration with
// the dialect's own policy, then delegate to the base build) so a refused
// configuration is exercised on the shipped rules.
class GlmModel final : public llm::chat_completions::ChatCompletionsModel {
public:
    GlmModel(asio::any_io_executor executor, nlohmann::json config)
        : ChatCompletionsModel(std::move(executor), std::move(config),
                               llm::glm::glm_dialect()) {}

    bool build() noexcept override {
        try {
            auto candidate = _config;
            llm::glm::prepare_generation(candidate);
            return ChatCompletionsModel::build();
        } catch (const std::exception&) {
            return false;
        }
    }
};

/// A mid-ReAct history: user question, one tool-calling assistant step that
/// carried reasoning, and its tool result — the exact shape interleaved
/// thinking with tools requires the replayed reasoning for.
model_io::AgentInputState tool_history() {
    model_io::AgentInputState state;

    model_io::Invocable weather;
    weather.name = "weather";
    weather.description = "Get the weather";
    weather.argument_schema = {
        {"type", "object"},
        {"properties", {{"city", {{"type", "string"}}}}},
    };
    state.tools.push_back(std::move(weather));

    model_io::UserLoopStep turn;
    turn.user_input.role = "user";
    model_io::Content question;
    question.type = model_io::ContentType::Text;
    question.raw = "Weather in Beijing?";
    turn.user_input.content.push_back(std::move(question));

    model_io::AgentLoopStep step;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    model_io::Content reasoning;
    reasoning.type = model_io::ContentType::Text;
    reasoning.raw = "prior chain";
    step.model_response.reasoning = std::move(reasoning);
    model_io::InvokeQuery query;
    query.id = "call_1";
    query.name = "weather";
    query.arguments = {{"city", "Beijing"}};
    step.model_response.invokes = std::vector<model_io::InvokeQuery>{query};

    model_io::MessageItem tool_result;
    tool_result.type = model_io::MessageItemType::InvokeReturn;
    model_io::Content tool_output;
    tool_output.type = model_io::ContentType::Text;
    tool_output.raw = "sunny";
    tool_result.content.push_back(tool_output);
    model_io::InvokeReturn provenance;
    provenance.query = query;
    provenance.output = tool_output;
    tool_result.invoke_return = provenance;
    step.invoke_returns = std::vector<model_io::MessageItem>{tool_result};
    turn.agent_loop_step.push_back(std::move(step));
    state.turns.push_back(std::move(turn));
    return state;
}

std::string sse(const nlohmann::json& chunk) {
    return "data: " + chunk.dump() + "\n\n";
}

nlohmann::json frame(nlohmann::json delta,
                     nlohmann::json finish_reason = nullptr) {
    return {
        {"id", "202610301513378312645bf8ab4d6c"},
        {"object", "chat.completion.chunk"},
        {"model", "glm-5.3-flash"},
        {"choices", nlohmann::json::array({{
            {"index", 0},
            {"delta", std::move(delta)},
            {"finish_reason", std::move(finish_reason)},
        }})},
    };
}

/// The usage trailer GLM sends on the final choice chunk (verified 2026-10-30).
nlohmann::json usage_frame(const char* finish_reason) {
    nlohmann::json chunk = frame(nlohmann::json::object(), finish_reason);
    chunk["usage"] = {
        {"prompt_tokens", 158},
        {"completion_tokens", 12},
        {"total_tokens", 170},
        {"prompt_tokens_details", {{"cached_tokens", 96}}},
        {"completion_tokens_details", {{"reasoning_tokens", 0}}},
    };
    return chunk;
}

/// serve_fixed_response's shape, minus discarding the request: the body is
/// captured so the test can assert what the dialect put on the wire.
class CapturingServer {
public:
    CapturingServer(std::string response_body)
        : _server([this, body = std::move(response_body)](
                      asio::ip::tcp::socket& socket) {
              beast::flat_buffer buffer;
              http::request<http::string_body> request;
              http::read(socket, buffer, request);
              captured = request.body();

              http::response<http::string_body> response(http::status::ok, 11);
              response.set(http::field::content_type, "text/event-stream");
              response.set(http::field::connection, "close");
              response.body() = body;
              response.prepare_payload();
              http::write(socket, response);
          }) {}

    unsigned short wait_listening() { return _server.wait_listening(); }
    void join() { _server.join(); }

    std::string captured;

private:
    loopback::OneShotServer _server;
};

nlohmann::json configuration(unsigned short port,
                             nlohmann::json extra = nlohmann::json::object()) {
    nlohmann::json config{
        {"model", "glm-5.3-flash"},
        {"retry", {{"max_attempts", 0}}},
        {"endpoint", {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
    };
    if (extra.is_object() && !extra.empty()) {
        config.update(extra);
    }
    return config;
}

} // namespace

BOOST_AUTO_TEST_CASE(converse_drives_the_full_glm_exchange) {
    const std::string stream =
        sse(frame({{"role", "assistant"}, {"reasoning_content", "thinking "}})) +
        sse(frame({{"reasoning_content", "hard"}})) +
        sse(frame({{"content", "Let me check."}})) +
        sse(frame({{"tool_calls", nlohmann::json::array({
             {{"index", 0}, {"id", "call_2"}, {"type", "function"},
              {"function", {{"name", "weather"},
                            {"arguments", "{\"city\":\"Beijing\"}"}}}},
         })}})) +
        sse(usage_frame("tool_calls")) +
        "data: [DONE]\n\n";

    CapturingServer server(stream);
    const auto port = server.wait_listening();

    asio::io_context io;
    // The host's responses-style envelope is what a portable configuration
    // carries; GLM takes the top-level field and refuses `medium`.
    GlmModel model(io.get_executor(), configuration(port, {
        {"reasoning", {{"effort", "medium"}}},
        {"temperature", 0.3},
    }));
    BOOST_REQUIRE(model.build());

    std::vector<llm::chat_completions::ReasoningDeltaEvent> broadcast;
    eventbus::EventBus::ScopedSubscription view =
        eventbus::default_bus()
            .subscribe<llm::chat_completions::ReasoningDeltaEvent>(
                [&](const llm::chat_completions::ReasoningDeltaEvent& e) {
                    broadcast.push_back(e);
                });

    std::optional<model_io::MessageItem> result;
    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            result = co_await model.converse(tool_history());
        } catch (...) {
            failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    server.join();
    if (failure) std::rethrow_exception(failure);

    // --- the request side: everything the dialect owes the wire ---------------
    const nlohmann::json body = nlohmann::json::parse(server.captured);
    BOOST_CHECK_EQUAL(body["model"], "glm-5.3-flash");
    // medium is the family's documented alias for high.
    BOOST_CHECK_EQUAL(body["reasoning_effort"], "high");
    BOOST_CHECK(!body.contains("reasoning"));
    BOOST_CHECK(!body.contains("n"));
    BOOST_CHECK(!body.contains("frequency_penalty"));
    BOOST_CHECK(!body.contains("presence_penalty"));
    BOOST_CHECK(!body.contains("thinking_budget"));
    // The shared builder's usage trailer survives: GLM accepts it and answers
    // with the usage block the cost accounting reads.
    BOOST_CHECK_EQUAL(body["stream_options"]["include_usage"], true);
    BOOST_CHECK_EQUAL(body["stream"], true);
    BOOST_CHECK_EQUAL(body["temperature"], 0.3);
    BOOST_CHECK_EQUAL(body["tools"][0]["function"]["name"], "weather");

    const auto& messages = body["messages"];
    BOOST_REQUIRE_EQUAL(messages.size(), 3u);
    BOOST_CHECK_EQUAL(messages[0]["role"], "user");
    BOOST_CHECK_EQUAL(messages[0]["content"], "Weather in Beijing?");
    const auto& assistant = messages[1];
    // Interleaved thinking with tools: the reasoning travels with the call.
    BOOST_CHECK_EQUAL(assistant["reasoning_content"], "prior chain");
    BOOST_CHECK(assistant["content"].is_null());
    BOOST_CHECK_EQUAL(assistant["tool_calls"][0]["function"]["name"],
                      "weather");
    BOOST_CHECK_EQUAL(messages[2]["role"], "tool");
    BOOST_CHECK_EQUAL(messages[2]["tool_call_id"], "call_1");
    BOOST_CHECK_EQUAL(messages[2]["content"], "sunny");

    // --- the response side: everything the reader owes the caller ------------
    BOOST_REQUIRE(result);
    BOOST_CHECK(result->type == model_io::MessageItemType::ModelResponse);
    BOOST_REQUIRE(result->reasoning);
    BOOST_CHECK_EQUAL(result->reasoning->raw, "thinking hard");
    BOOST_CHECK_EQUAL(result->content[0].raw, "Let me check.");
    BOOST_REQUIRE(result->invokes);
    BOOST_REQUIRE_EQUAL(result->invokes->size(), 1u);
    BOOST_CHECK_EQUAL((*result->invokes)[0].id, "call_2");
    BOOST_CHECK_EQUAL((*result->invokes)[0].name, "weather");
    BOOST_CHECK_EQUAL((*result->invokes)[0].arguments["city"], "Beijing");
    BOOST_REQUIRE(result->cost);
    BOOST_CHECK_EQUAL(result->cost->prompt, 158u);
    BOOST_CHECK_EQUAL(result->cost->generated, 12u);
    BOOST_CHECK_EQUAL(result->cost->cache_hit, 96u);
    BOOST_REQUIRE(result->extras);
    BOOST_CHECK_EQUAL((*result->extras)["finish_reason"], "tool_calls");
    BOOST_CHECK_EQUAL((*result->extras)["model"], "glm-5.3-flash");

    // --- the broadcast side: the live view saw the same increments ----------
    BOOST_REQUIRE_EQUAL(broadcast.size(), 2u);
    BOOST_CHECK_EQUAL(broadcast[0].reasoning, "thinking ");
    BOOST_CHECK_EQUAL(broadcast[1].reasoning, "hard");
    BOOST_CHECK_EQUAL(broadcast[0].provider, "glm");
    BOOST_CHECK_EQUAL(broadcast[0].model, "glm-5.3-flash");
}

// A moderation stop arrives as finish_reason `sensitive`. Without the mapping
// the shared reader would report a generic failure for a completed exchange.
BOOST_AUTO_TEST_CASE(sensitive_finish_maps_to_the_content_filter_verdict) {
    const std::string stream =
        sse(frame({{"content", "partial"}})) +
        sse(usage_frame("sensitive")) +
        "data: [DONE]\n\n";

    CapturingServer server(stream);
    const auto port = server.wait_listening();

    asio::io_context io;
    GlmModel model(io.get_executor(), configuration(port));
    BOOST_REQUIRE(model.build());

    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            co_await model.converse(tool_history());
        } catch (...) {
            failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    server.join();
    BOOST_REQUIRE(failure);

    try {
        std::rethrow_exception(failure);
        BOOST_FAIL("expected ChatCompletionsApiException");
    } catch (const llm::chat_completions::ChatCompletionsApiException& error) {
        BOOST_CHECK(error.status() ==
                    llm::chat_completions::ChatCompletionStatus::ContentFiltered);
    }
}

// GLM's own error envelope: a string business code inside `error`.
BOOST_AUTO_TEST_CASE(converse_surfaces_api_error_chunks) {
    // The verbatim body of a live 1210 (thinking disabled on a 5.3 model).
    const std::string stream = sse({
        {"error",
         {{"code", "1210"},
          {"message", "该模型始终思考，不支持关闭思考；请使用 low、high 或 max。"}}},
    });
    CapturingServer server(stream);
    const auto port = server.wait_listening();

    asio::io_context io;
    GlmModel model(io.get_executor(), configuration(port));
    BOOST_REQUIRE(model.build());

    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            co_await model.converse(tool_history());
        } catch (...) {
            failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    server.join();
    BOOST_REQUIRE(failure);

    try {
        std::rethrow_exception(failure);
        BOOST_FAIL("expected ChatCompletionsApiException");
    } catch (const llm::chat_completions::ChatCompletionsApiException& error) {
        BOOST_CHECK(error.status() ==
                    llm::chat_completions::ChatCompletionStatus::Failed);
        BOOST_CHECK_EQUAL(error.details()["code"], "1210");
    }
}

// What the service would reject with HTTP 400 is rejected before the instance
// is exposed, so a startup configuration cannot fail on its first exchange.
BOOST_AUTO_TEST_CASE(build_refuses_a_configuration_the_service_rejects) {
    asio::io_context io;

    GlmModel disabled(io.get_executor(), nlohmann::json{
        {"model", "glm-5.3-flash"},
        {"thinking", {{"type", "disabled"}}},
    });
    BOOST_CHECK(!disabled.build());

    GlmModel minimal(io.get_executor(), nlohmann::json{
        {"model", "glm-5.3-flash"},
        {"reasoning", {{"effort", "minimal"}}},
    });
    BOOST_CHECK(!minimal.build());

    // A model outside the family keeps the permissive path.
    GlmModel other(io.get_executor(), nlohmann::json{
        {"model", "glm-4.6"},
        {"thinking", {{"type", "disabled"}}},
    });
    BOOST_CHECK(other.build());
}
