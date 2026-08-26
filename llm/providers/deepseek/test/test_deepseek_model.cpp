/**
 * @file test_deepseek_model.cpp
 * @brief Full-flow converse() tests for the deepseek provider over a loopback
 *        HTTP server.
 *
 * The exchange runs against a scripted SSE stream on 127.0.0.1: the server
 * captures the request body, so every dialect policy is asserted on the wire
 * (thinking default, effort clamp, parameter stripping, reasoning replay,
 * the usage-trailer request), and the assembled MessageItem is asserted on
 * the way back (reasoning slot, parsed invokes, cache-hit cost accounting).
 * The error-chunk path covers the chat layer's ApiException surfacing.
 */

#define BOOST_TEST_MODULE deepseek_model
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
#include "llm/chat_completions/events.hpp"
#include "llm/chat_completions/model.hpp"
#include "llm/deepseek/dialect.hpp"
#include "loopback_server.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

namespace {

// ChatCompletionsModel's constructor is protected: instances are minted by
// the plugin factory. The test mints its own with the real dialect.
class DeepSeekModel final : public llm::chat_completions::ChatCompletionsModel {
public:
    DeepSeekModel(asio::any_io_executor executor, nlohmann::json config)
        : ChatCompletionsModel(std::move(executor), std::move(config),
                               llm::deepseek::deepseek_dialect()) {}
};

/// A mid-ReAct history: user question, one tool-calling assistant step that
/// carried reasoning, and its tool result — the exact shape the thinking-mode
/// replay rule is about.
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
    question.raw = "Weather in Paris?";
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
    query.arguments = {{"city", "Paris"}};
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
        {"id", "chatcmpl_ds"},
        {"object", "chat.completion.chunk"},
        {"model", "deepseek-v4-flash"},
        {"choices", nlohmann::json::array({{
            {"index", 0},
            {"delta", std::move(delta)},
            {"finish_reason", std::move(finish_reason)},
        }})},
    };
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

} // namespace

BOOST_AUTO_TEST_CASE(converse_drives_the_full_deepseek_exchange) {
    const std::string stream =
        sse(frame({{"role", "assistant"}, {"reasoning_content", "thinking "}})) +
        sse(frame({{"reasoning_content", "hard"}})) +
        sse(frame({{"content", "Let me check."}})) +
        sse(frame({{"tool_calls", nlohmann::json::array({
             {{"index", 0}, {"id", "call_2"}, {"type", "function"},
              {"function", {{"name", "weather"},
                            {"arguments", "{\"city\":\"Paris\"}"}}}},
         })}})) +
        sse(frame(nlohmann::json::object(), "tool_calls")) +
        sse({
            {"id", "chatcmpl_ds"},
            {"object", "chat.completion.chunk"},
            {"model", "deepseek-v4-flash"},
            {"choices", nlohmann::json::array()},
            {"usage", {
                {"prompt_tokens", 20},
                {"completion_tokens", 6},
                {"total_tokens", 26},
                {"prompt_cache_hit_tokens", 14},
                {"prompt_cache_miss_tokens", 6},
            }},
        }) +
        "data: [DONE]\n\n";

    CapturingServer server(stream);
    const auto port = server.wait_listening();

    asio::io_context io;
    DeepSeekModel model(io.get_executor(), nlohmann::json{
        {"model", "deepseek-v4-flash"},
        {"reasoning", {{"effort", "low"}}},
        {"temperature", 0.3},
        {"retry", {{"max_attempts", 0}}},
        {"endpoint", {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
    });
    BOOST_REQUIRE(model.build());

    // The live-view contract: converse() broadcasts each reasoning increment
    // on the process-wide bus, synchronously in wire order, tagged with one
    // id per exchange plus the provider and model names.
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
    BOOST_CHECK_EQUAL(body["model"], "deepseek-v4-flash");
    BOOST_CHECK_EQUAL(body["thinking"]["type"], "enabled");
    BOOST_CHECK_EQUAL(body["reasoning_effort"], "high"); // low → high
    BOOST_CHECK(!body.contains("n"));
    BOOST_CHECK(!body.contains("frequency_penalty"));
    BOOST_CHECK_EQUAL(body["stream_options"]["include_usage"], true);
    BOOST_CHECK_EQUAL(body["temperature"], 0.3);
    BOOST_CHECK_EQUAL(body["tools"][0]["function"]["name"], "weather");

    const auto& messages = body["messages"];
    BOOST_REQUIRE_EQUAL(messages.size(), 3u);
    BOOST_CHECK_EQUAL(messages[0]["role"], "user");
    BOOST_CHECK_EQUAL(messages[0]["content"], "Weather in Paris?");
    const auto& assistant = messages[1];
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
    BOOST_CHECK_EQUAL((*result->invokes)[0].arguments["city"], "Paris");
    BOOST_REQUIRE(result->cost);
    BOOST_CHECK_EQUAL(result->cost->prompt, 20u);
    BOOST_CHECK_EQUAL(result->cost->generated, 6u);
    BOOST_CHECK_EQUAL(result->cost->cache_hit, 14u); // prompt_cache_hit_tokens
    BOOST_REQUIRE(result->extras);
    BOOST_CHECK_EQUAL((*result->extras)["finish_reason"], "tool_calls");
    BOOST_CHECK_EQUAL((*result->extras)["model"], "deepseek-v4-flash");

    // --- the broadcast side: the live view saw the same increments ----------
    BOOST_REQUIRE_EQUAL(broadcast.size(), 2u);
    BOOST_CHECK_EQUAL(broadcast[0].reasoning, "thinking ");
    BOOST_CHECK_EQUAL(broadcast[1].reasoning, "hard");
    BOOST_CHECK(!broadcast[0].reasoning_id.empty());
    BOOST_CHECK_EQUAL(broadcast[1].reasoning_id, broadcast[0].reasoning_id);
    BOOST_CHECK_EQUAL(broadcast[0].provider, "deepseek");
    BOOST_CHECK_EQUAL(broadcast[0].model, "deepseek-v4-flash");
}

BOOST_AUTO_TEST_CASE(converse_surfaces_api_error_chunks_as_chat_completions_api_exception) {
    const std::string stream =
        sse({{"error",
              {{"message", "Insufficient Balance"},
               {"type", "insufficient_balance"},
               {"code", 402}}}});
    CapturingServer server(stream);
    const auto port = server.wait_listening();

    asio::io_context io;
    DeepSeekModel model(io.get_executor(), nlohmann::json{
        {"model", "deepseek-v4-flash"},
        {"retry", {{"max_attempts", 0}}},
        {"endpoint", {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
    });
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
        BOOST_CHECK_EQUAL(error.details()["message"], "Insufficient Balance");
        BOOST_CHECK_EQUAL(error.what(), "Insufficient Balance");
    }
}
