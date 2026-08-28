/**
 * @file test_deepseek_plugin.cpp
 * @brief The dynamic-loading DeepSeek provider tests: registry routing over
 *        the real .so, plus one full exchange driven across the dlopen
 *        boundary.
 *
 * The loopback exchange is the regression guard for the cross-boundary
 * runtime wiring: this executable's io_context runs a chain that lives in
 * the plugin .so, and the reasoning increments the plugin broadcasts must
 * arrive at a subscriber in THIS module. A relapse to per-module Asio
 * (header-only runtime forked across the boundary) crashed the plugin's
 * reactor callbacks intermittently in live use; a forked event bus would
 * silently drop the events. Both shared libraries — libasio.so,
 * libeventbus.so — exist to make this test's world hold together.
 *
 * The ABI-v2/v3 surface rides the same guard: provider_info()'s
 * awaitable<nlohmann::json> crosses back out of the .so by value, and both
 * set_generation() tiers (typed preset + JSON merge-patch) cross in — the
 * captured request body must show the patched knobs.
 */

#define BOOST_TEST_MODULE deepseek_llm_plugin
#include <boost/test/unit_test.hpp>

#include <boost/asio/io_context.hpp>

#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "eventbus/event_bus.hpp"
#include "llm/chat_completions/events.hpp"
#include "llm/models.hpp"
#include "loopback_server.hpp"

#ifndef DEEPSEEK_PLUGIN_DIR
#error DEEPSEEK_PLUGIN_DIR must name the built provider-plugin directory
#endif

BOOST_AUTO_TEST_CASE(dispatcher_loads_and_mints_deepseek_chat_model) {
    llm::LLMDispatcher dispatcher;
    // The plugins/llm directory carries every bundled provider .so — openai
    // and deepseek today; keep this count in sync when a provider joins.
    BOOST_CHECK_EQUAL(
        dispatcher.load_models(std::filesystem::path(DEEPSEEK_PLUGIN_DIR)), 2u);

    boost::asio::io_context io;
    auto model = dispatcher.create_model(
        "deepseek", io.get_executor(),
        nlohmann::json{{"model", "deepseek-v4-flash"}});
    BOOST_REQUIRE(model);
    BOOST_CHECK(model->model_type() == llm::LLMModelType::Conversation);

    // The neighbours coexist: both providers stay routable after loading.
    BOOST_CHECK(dispatcher.create_model(
        "openai", io.get_executor(),
        nlohmann::json{{"model", "test-model"}}));
    BOOST_CHECK(!dispatcher.create_model(
        "unknown", io.get_executor(),
        nlohmann::json{{"model", "test-model"}}));
}

namespace {

std::string sse(const nlohmann::json& chunk) {
    return "data: " + chunk.dump() + "\n\n";
}

/// One streamed exchange in the live DeepSeek shape (usage rides the finish
/// chunk; see test_chat_completions_stream's live-shape fixtures).
std::string one_reasoning_exchange() {
    const auto frame = [](nlohmann::json delta, nlohmann::json finish = nullptr,
                          nlohmann::json usage = nullptr) {
        return nlohmann::json{
            {"id", "chatcmpl_plugin"},
            {"object", "chat.completion.chunk"},
            {"model", "deepseek-v4-flash"},
            {"choices", nlohmann::json::array({{
                {"index", 0},
                {"delta", std::move(delta)},
                {"finish_reason", std::move(finish)},
            }})},
            {"usage", std::move(usage)},
        };
    };
    return sse(frame({{"role", "assistant"}, {"reasoning_content", "thinking "}})) +
           sse(frame({{"reasoning_content", "hard"}})) +
           sse(frame({{"content", "OK"}}, "stop", {
               {"prompt_tokens", 9},
               {"completion_tokens", 1},
               {"total_tokens", 10},
               {"prompt_tokens_details", {{"cached_tokens", 0}}},
           })) +
           "data: [DONE]\n\n";
}

/// One user turn carrying a single text content.
model_io::AgentInputState one_user_turn(std::string text) {
    model_io::AgentInputState state;
    model_io::MessageItem input;
    input.type = model_io::MessageItemType::UserInput;
    input.role = "user";
    model_io::Content content;
    content.type = model_io::ContentType::Text;
    content.raw = std::move(text);
    input.content.push_back(std::move(content));
    state.turns.push_back({});
    state.turns.back().user_input = std::move(input);
    return state;
}

/// serve_fixed_response's shape, but the request body is handed back — the
/// capture the set_generation tests assert on.
std::string capture_and_serve(tcp::socket& socket,
                               const std::string& response_body) {
    beast::flat_buffer buffer;
    http::request<http::string_body> request;
    http::read(socket, buffer, request);

    http::response<http::string_body> response(http::status::ok, 11);
    response.set(http::field::content_type, "text/event-stream");
    response.set(http::field::connection, "close");
    response.body() = response_body;
    response.prepare_payload();
    http::write(socket, response);
    return request.body();
}

} // namespace

BOOST_AUTO_TEST_CASE(dlopened_plugin_drives_a_loopback_exchange) {
    llm::LLMDispatcher dispatcher;
    BOOST_REQUIRE_EQUAL(
        dispatcher.load_models(std::filesystem::path(DEEPSEEK_PLUGIN_DIR)), 2u);

    loopback::OneShotServer server([](boost::asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(
            socket, boost::beast::http::status::ok, one_reasoning_exchange());
    });
    const auto port = server.wait_listening();

    boost::asio::io_context io;
    auto model = dispatcher.create_model(
        "deepseek", io.get_executor(),
        nlohmann::json{
            {"model", "deepseek-v4-flash"},
            {"reasoning", {{"effort", "high"}}},
            {"retry", {{"max_attempts", 0}}},
            {"endpoint",
             {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
        });
    BOOST_REQUIRE(model);

    std::vector<llm::chat_completions::ReasoningDeltaEvent> broadcast;
    eventbus::EventBus::ScopedSubscription view =
        eventbus::default_bus()
            .subscribe<llm::chat_completions::ReasoningDeltaEvent>(
                [&](const llm::chat_completions::ReasoningDeltaEvent& event) {
                    broadcast.push_back(event);
                });

    model_io::AgentInputState state;
    model_io::MessageItem input;
    input.type = model_io::MessageItemType::UserInput;
    input.role = "user";
    model_io::Content text;
    text.type = model_io::ContentType::Text;
    text.raw = "reply OK";
    input.content.push_back(std::move(text));
    state.turns.push_back({});
    state.turns.back().user_input = std::move(input);

    std::optional<model_io::MessageItem> result;
    std::exception_ptr failure;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            try {
                result = co_await model->converse(state);
            } catch (...) {
                failure = std::current_exception();
            }
        },
        boost::asio::detached);
    io.run();
    server.join();
    if (failure) std::rethrow_exception(failure);

    // The exchange itself assembled through the plugin's chain.
    BOOST_REQUIRE(result);
    BOOST_REQUIRE_EQUAL(result->content.size(), 1u);
    BOOST_CHECK_EQUAL(result->content.front().raw, "OK");
    BOOST_REQUIRE(result->reasoning);
    BOOST_CHECK_EQUAL(result->reasoning->raw, "thinking hard");
    BOOST_REQUIRE(result->cost);
    BOOST_CHECK_EQUAL(result->cost->prompt, 9u);

    // The reasoning crossed back over the boundary on the process bus:
    // in wire order, one stable id, tagged with provider and model.
    BOOST_REQUIRE_EQUAL(broadcast.size(), 2u);
    BOOST_CHECK_EQUAL(broadcast[0].reasoning, "thinking ");
    BOOST_CHECK_EQUAL(broadcast[1].reasoning, "hard");
    BOOST_CHECK(!broadcast[0].reasoning_id.empty());
    BOOST_CHECK_EQUAL(broadcast[0].reasoning_id, broadcast[1].reasoning_id);
    BOOST_CHECK_EQUAL(broadcast[0].provider, "deepseek");
    BOOST_CHECK_EQUAL(broadcast[0].model, "deepseek-v4-flash");
}

// provider_info() across the boundary: the coroutine and its connect+GET
// chain live behind the plugin's vtable slot, the result crosses back out
// of the .so as one nlohmann::json by value. DeepSeek's dialect attaches
// the balance companion, so the object shape — models + balance, two
// sequential GETs — is what must survive the crossing.
BOOST_AUTO_TEST_CASE(dlopened_plugin_serves_provider_info_with_balance) {
    llm::LLMDispatcher dispatcher;
    BOOST_REQUIRE_EQUAL(
        dispatcher.load_models(std::filesystem::path(DEEPSEEK_PLUGIN_DIR)), 2u);

    const std::string catalogue = nlohmann::json{
        {"object", "list"},
        {"data", nlohmann::json::array({
            {{"id", "deepseek-chat"}, {"object", "model"},
             {"owned_by", "deepseek"}},
            {{"id", "deepseek-reasoner"}, {"object", "model"},
             {"owned_by", "deepseek"}},
        })},
    }.dump();
    const std::string balance = nlohmann::json{
        {"is_available", true},
        {"balance_infos", nlohmann::json::array({
            {{"currency", "CNY"},
             {"total_balance", "110.00"},
             {"granted_balance", "10.00"},
             {"topped_up_balance", "100.00"}},
        })},
    }.dump();
    loopback::SequenceServer server({
        [&](tcp::socket& socket) {
            loopback::serve_fixed_response(
                socket, http::status::ok, catalogue);
        },
        [&](tcp::socket& socket) {
            loopback::serve_fixed_response(
                socket, http::status::ok, balance);
        },
    });
    const auto port = server.wait_listening();

    boost::asio::io_context io;
    auto model = dispatcher.create_model(
        "deepseek", io.get_executor(),
        nlohmann::json{
            {"model", "deepseek-v4-flash"},
            {"retry", {{"max_attempts", 0}}},
            {"endpoint",
             {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
        });
    BOOST_REQUIRE(model);

    std::optional<nlohmann::json> result;
    std::exception_ptr failure;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            try {
                result = co_await model->provider_info();
            } catch (...) {
                failure = std::current_exception();
            }
        },
        boost::asio::detached);
    io.run();
    server.join();
    if (failure) std::rethrow_exception(failure);

    BOOST_REQUIRE(result);
    BOOST_REQUIRE(result->is_object());
    BOOST_REQUIRE((*result)["models"].is_array());
    BOOST_REQUIRE_EQUAL((*result)["models"].size(), 2u);
    BOOST_CHECK_EQUAL((*result)["models"][0]["id"], "deepseek-chat");
    BOOST_CHECK_EQUAL((*result)["models"][1]["id"], "deepseek-reasoner");
    BOOST_CHECK_EQUAL((*result)["balance"]["is_available"], true);
    BOOST_CHECK_EQUAL(
        (*result)["balance"]["balance_infos"][0]["currency"], "CNY");
}

// set_generation() across the boundary — both tiers — then converse(): the
// patched knobs must ride the request body the loopback server receives.
BOOST_AUTO_TEST_CASE(dlopened_model_generation_knobs_reach_the_wire) {
    llm::LLMDispatcher dispatcher;
    BOOST_REQUIRE_EQUAL(
        dispatcher.load_models(std::filesystem::path(DEEPSEEK_PLUGIN_DIR)), 2u);

    const auto captured = std::make_shared<std::string>();
    loopback::OneShotServer server([captured](tcp::socket& socket) {
        *captured = capture_and_serve(socket, one_reasoning_exchange());
    });
    const auto port = server.wait_listening();

    boost::asio::io_context io;
    auto model = dispatcher.create_model(
        "deepseek", io.get_executor(),
        nlohmann::json{
            {"model", "deepseek-v4-flash"},
            {"temperature", 0.1},
            {"retry", {{"max_attempts", 0}}},
            {"endpoint",
             {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
        });
    BOOST_REQUIRE(model);

    // Both tiers cross the boundary: the typed preset (model + effort)
    // and the JSON merge-patch (one detail knob).
    model->set_generation(llm::GenerationPreset{
        .model = "deepseek-v4-reasoner",
        .effort = llm::ReasoningEffort::Medium,
    });
    model->set_generation(nlohmann::json{{"temperature", 0.9}});

    std::optional<model_io::MessageItem> result;
    std::exception_ptr failure;
    boost::asio::co_spawn(
        io,
        [&]() -> boost::asio::awaitable<void> {
            try {
                result = co_await model->converse(one_user_turn("reply OK"));
            } catch (...) {
                failure = std::current_exception();
            }
        },
        boost::asio::detached);
    io.run();
    server.join();
    if (failure) std::rethrow_exception(failure);

    BOOST_REQUIRE(result);
    BOOST_CHECK_EQUAL(result->content.front().raw, "OK");
    const auto body = nlohmann::json::parse(*captured);
    BOOST_CHECK_EQUAL(body["model"], "deepseek-v4-reasoner");
    BOOST_CHECK_EQUAL(body["reasoning_effort"], "medium");
    BOOST_CHECK_EQUAL(body["temperature"], 0.9);
}
