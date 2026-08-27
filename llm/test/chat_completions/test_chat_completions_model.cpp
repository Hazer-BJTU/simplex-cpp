#define BOOST_TEST_MODULE chat_completions_model
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "endpoint/http_request_exception.hpp"
#include "llm/chat_completions/interpreter.hpp"
#include "llm/chat_completions/model.hpp"
#include "loopback_server.hpp"

namespace asio = boost::asio;
namespace http = boost::beast::http;

namespace {

class FixtureDialect final : public llm::chat_completions::ChatCompletionsDialect {
public:
    std::string_view provider_name() const override { return "fixture"; }
};

class FixtureModel final : public llm::chat_completions::ChatCompletionsModel {
public:
    FixtureModel(asio::any_io_executor executor, nlohmann::json config,
                 llm::chat_completions::ChatCompletionsDialectPtr dialect =
                     std::make_shared<const FixtureDialect>())
        : ChatCompletionsModel(std::move(executor), std::move(config),
                               std::move(dialect)) {}
};

/// One provider_info() drive on its own io_context (the model must be bound
/// to the executor that runs). Returns whichever side of the outcome
/// materialised; the caller asserts and joins the server.
struct ProviderInfoOutcome {
    std::optional<nlohmann::json> result;
    std::exception_ptr failure;
};

ProviderInfoOutcome run_provider_info(
    const nlohmann::json& config,
    llm::chat_completions::ChatCompletionsDialectPtr dialect =
        std::make_shared<const FixtureDialect>()) {
    asio::io_context io;
    FixtureModel model(io.get_executor(), config, std::move(dialect));
    BOOST_REQUIRE(model.build());

    ProviderInfoOutcome outcome;
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            outcome.result = co_await model.provider_info();
        } catch (...) {
            outcome.failure = std::current_exception();
        }
    }, asio::detached);
    io.run();
    return outcome;
}

nlohmann::json loopback_config(unsigned short port) {
    return nlohmann::json{
        {"model", "fixture-model"},
        {"endpoint", {{"base_url", "http://127.0.0.1:" + std::to_string(port)}}},
    };
}

} // namespace

// ---------------------------------------------------------------------------
// set_generation on the adapter's build() derivation
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(set_generation_layers_on_the_build_derivation) {
    asio::io_context io;
    FixtureModel model(io.get_executor(), nlohmann::json{
        {"model", "fixture-model"},
        {"temperature", 0.2},
        // Never dialed: this test does no I/O.
        {"endpoint", {{"base_url", "http://127.0.0.1:9"}}},
    });
    BOOST_REQUIRE(model.build());
    // build() derived the knobs: host keys stripped, the rest verbatim.
    BOOST_CHECK(!model.generation().contains("endpoint"));
    BOOST_CHECK_EQUAL(model.generation()["temperature"], 0.2);

    model.set_generation(nlohmann::json{{"temperature", 0.9}});
    BOOST_CHECK_EQUAL(model.generation()["temperature"], 0.9);

    // The idempotent second build() (_built guard) must not reset the knobs.
    BOOST_CHECK(model.build());
    BOOST_CHECK_EQUAL(model.generation()["temperature"], 0.9);
}

BOOST_AUTO_TEST_CASE(preset_reaches_the_built_request_body) {
    asio::io_context io;
    FixtureModel model(io.get_executor(), nlohmann::json{
        {"model", "fixture-model"},
        {"endpoint", {{"base_url", "https://example.invalid"}}},
    });
    BOOST_REQUIRE(model.build());

    model.set_generation(llm::GenerationPreset{
        .model = "switched-model",
        .effort = llm::ReasoningEffort::High,
    });

    llm::chat_completions::ChatCompletionsInterpreter interpreter(
        std::make_shared<const FixtureDialect>());
    model_io::AgentInputState state;   // empty conversation is fine
    const auto request = interpreter.build_request(
        state, model.endpoint(), model.generation());
    const auto body = nlohmann::json::parse(request.body());

    BOOST_CHECK_EQUAL(body["model"], "switched-model");
    // The typed tier's envelope, folded into the protocol's top-level
    // spelling by the interpreter's translate_reasoning_envelope.
    BOOST_CHECK_EQUAL(body["reasoning_effort"], "high");
    BOOST_CHECK(!body.contains("reasoning"));
}

// ---------------------------------------------------------------------------
// provider_info over the loopback transport
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(provider_info_normalises_the_catalogue_envelope) {
    const std::string catalogue = nlohmann::json{
        {"object", "list"},
        {"data", nlohmann::json::array({
            {{"id", "fixture-mini"}, {"object", "model"}, {"owned_by", "fixture"}},
            {{"id", "fixture-pro"}, {"object", "model"}, {"owned_by", "fixture"}},
        })},
    }.dump();
    loopback::OneShotServer server([&](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, catalogue);
    });

    auto outcome = run_provider_info(loopback_config(server.wait_listening()));
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_REQUIRE(outcome.result->is_array());
    BOOST_REQUIRE_EQUAL(outcome.result->size(), 2u);
    BOOST_CHECK_EQUAL((*outcome.result)[0]["id"], "fixture-mini");
    BOOST_CHECK_EQUAL((*outcome.result)[1]["id"], "fixture-pro");
}

BOOST_AUTO_TEST_CASE(provider_info_passes_a_top_level_array_through) {
    const std::string catalogue = nlohmann::json::array({
        {{"id", "solo"}},
    }).dump();
    loopback::OneShotServer server([&](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok, catalogue);
    });

    auto outcome = run_provider_info(loopback_config(server.wait_listening()));
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_REQUIRE(outcome.result->is_array());
    BOOST_REQUIRE_EQUAL(outcome.result->size(), 1u);
    BOOST_CHECK_EQUAL((*outcome.result)[0]["id"], "solo");
}

BOOST_AUTO_TEST_CASE(provider_info_surfaces_non_200_answers) {
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::unauthorized,
                                       R"({"error": "bad key"})");
    });

    auto outcome = run_provider_info(loopback_config(server.wait_listening()));
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::HandleResponse);
        BOOST_CHECK_EQUAL(e.status(), 401u);
    } catch (...) {
        BOOST_FAIL("expected an HttpRequestException for the 401 catalogue");
    }
}

BOOST_AUTO_TEST_CASE(provider_info_rejects_an_unexpected_payload_shape) {
    // 200, JSON, but neither an array nor the {"data": [...]} catalogue.
    loopback::OneShotServer server([](asio::ip::tcp::socket& socket) {
        loopback::serve_fixed_response(socket, http::status::ok,
                                       R"({"object": "list"})");
    });

    auto outcome = run_provider_info(loopback_config(server.wait_listening()));
    server.join();

    BOOST_REQUIRE(outcome.failure);
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const HttpRequestException& e) {
        BOOST_CHECK(e.stage() == HttpRequestException::Stage::HandleResponse);
        BOOST_CHECK_EQUAL(e.status(), 0u);
    } catch (...) {
        BOOST_FAIL("expected an HttpRequestException for the bad shape");
    }
}

// A dialect exposing a balance companion widens provider_info() to one
// object: the models array under "models", the provider's balance document
// verbatim under "balance" — two sequential GETs over the same endpoint,
// catalogue first (the server's sequence order pins the request order).
BOOST_AUTO_TEST_CASE(provider_info_attaches_the_balance_companion) {
    class BalancedDialect final
        : public llm::chat_completions::ChatCompletionsDialect {
    public:
        std::string balance_path() const override { return "/fixture/balance"; }
    };

    const std::string catalogue = nlohmann::json{
        {"object", "list"},
        {"data", nlohmann::json::array({
            {{"id", "fixture-mini"}, {"object", "model"}, {"owned_by", "fixture"}},
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
        [&](asio::ip::tcp::socket& socket) {
            loopback::serve_fixed_response(socket, http::status::ok, catalogue);
        },
        [&](asio::ip::tcp::socket& socket) {
            loopback::serve_fixed_response(socket, http::status::ok, balance);
        },
    });

    auto outcome = run_provider_info(
        loopback_config(server.wait_listening()),
        std::make_shared<const BalancedDialect>());
    server.join();

    if (outcome.failure) std::rethrow_exception(outcome.failure);
    BOOST_REQUIRE(outcome.result);
    BOOST_REQUIRE(outcome.result->is_object());
    BOOST_REQUIRE((*outcome.result)["models"].is_array());
    BOOST_REQUIRE_EQUAL((*outcome.result)["models"].size(), 1u);
    BOOST_CHECK_EQUAL((*outcome.result)["models"][0]["id"], "fixture-mini");
    BOOST_CHECK_EQUAL((*outcome.result)["balance"]["is_available"], true);
    BOOST_CHECK_EQUAL(
        (*outcome.result)["balance"]["balance_infos"][0]["currency"], "CNY");
    BOOST_CHECK_EQUAL(
        (*outcome.result)["balance"]["balance_infos"][0]["total_balance"],
        "110.00");
}
