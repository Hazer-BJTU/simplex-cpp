/**
 * @file test_models.cpp
 * @brief In-memory unit tests for the llm model-plugin contract (llm/models.hpp).
 *
 * Covers the core contract with in-memory contexts. The concrete provider
 * dynamic path is exercised separately by test_openai_plugin.cpp.
 *
 *   - LLMModelType vocabulary: every value distinct, tail-appendable policy.
 *   - LLMModel defaults: converse() throws LLMUnsupportedOperation; the
 *     integrate() placement table (UserInput -> new turn, ModelResponse ->
 *     new agent step on the current turn, InvokeReturn -> current step's
 *     invoke_returns, orphan folds create host structures) and the
 *     persistence guarantee (folded state stays to_json-serialisable).
 *   - The create_model lifecycle guarantee: build() exactly once before use,
 *     release() exactly once before destruction, config injected verbatim,
 *     build() failure -> nullptr with no release().
 *   - LLMDispatcher over in-memory contexts: routing by provider name,
 *     unknown provider -> nullptr, and the admission gates (ABI mismatch,
 *     non-llm context) enforced through create_model() too.
 */

#define BOOST_TEST_MODULE LlmModelsTests
#include <boost/test/unit_test.hpp>

#include "llm/models.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

/// A minimal Conversation model: only model_type(), everything else default.
struct BareModel : llm::LLMModel {
    BareModel(boost::asio::any_io_executor executor, nlohmann::json config)
        : LLMModel(std::move(executor), std::move(config)) {}
    llm::LLMModelType model_type() const noexcept override {
        return llm::LLMModelType::Conversation;
    }
    /// Test access to the protected injected configuration.
    const nlohmann::json& config() const { return _config; }
};

/// A model that records the lifecycle calls (counters are static: the test
/// inspects them after the shared_ptr has been destroyed, when instance
/// members would be dangling).
struct CountingModel : BareModel {
    using BareModel::BareModel;
    static inline int builds = 0;
    static inline int releases = 0;
    bool build() noexcept override { ++builds; return true; }
    void release() noexcept override { ++releases; }
};

/// A model whose build() fails — create_model must refuse it.
struct FailingBuildModel : BareModel {
    using BareModel::BareModel;
    static inline int builds = 0;
    static inline int releases = 0;
    bool build() noexcept override { ++builds; return false; }
    void release() noexcept override { ++releases; }
};

/// Which concrete model a FakeContext mints.
enum class MintedKind { Counting, FailingBuild };

/// In-memory llm descriptor: overrides mint() so no dynamic library is
/// needed, with configurable name and ABI for the gate tests.
struct FakeContext : llm::LLMModelExtensionContext {
    std::string n;
    std::uint32_t abi = llm::LLM_PLUGIN_ABI_VERSION;
    MintedKind kind = MintedKind::Counting;

    std::uint32_t abi_version() const noexcept override { return abi; }
    std::string_view name() const noexcept override { return n; }

protected:
    std::unique_ptr<llm::LLMModel> mint(
        boost::asio::any_io_executor executor,
        const nlohmann::json& config) const noexcept override {
        try {
            if (kind == MintedKind::FailingBuild) {
                return std::make_unique<FailingBuildModel>(std::move(executor), config);
            }
            return std::make_unique<CountingModel>(std::move(executor), config);
        } catch (...) {
            return nullptr;
        }
    }
};

/// A context that is NOT an llm descriptor (wrong-type gate).
struct NonLlmContext : extension::ExtensionContext {
    std::uint32_t abi_version() const noexcept override { return llm::LLM_PLUGIN_ABI_VERSION; }
    std::string_view name() const noexcept override { return "alien"; }
};

nlohmann::json sample_config(std::string provider) {
    return nlohmann::json{
        {"provider", std::move(provider)},
        {"endpoint", nlohmann::json{{"base_url", "https://example.invalid"}}},
    };
}

/// A UserInput item for fold tests.
model_io::MessageItem user_item(std::string text) {
    model_io::MessageItem item;
    item.type = model_io::MessageItemType::UserInput;
    item.role = "user";
    item.content.emplace_back().raw = std::move(text);
    return item;
}

/// A ModelResponse item for fold tests.
model_io::MessageItem response_item(std::string text) {
    model_io::MessageItem item;
    item.type = model_io::MessageItemType::ModelResponse;
    item.role = "assistant";
    item.content.emplace_back().raw = std::move(text);
    return item;
}

/// An InvokeReturn item for fold tests.
model_io::MessageItem result_item(std::string text) {
    model_io::MessageItem item;
    item.type = model_io::MessageItemType::InvokeReturn;
    item.role = "tool";
    item.content.emplace_back().raw = std::move(text);
    return item;
}

} // namespace

BOOST_AUTO_TEST_SUITE(LlmModelsSuite)

// ---------------------------------------------------------------------------
// LLMModelType vocabulary
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(model_type_values_are_distinct) {
    const std::vector<llm::LLMModelType> all = {
        llm::LLMModelType::Conversation,    llm::LLMModelType::Generation,
        llm::LLMModelType::Embedding,       llm::LLMModelType::Rerank,
        llm::LLMModelType::ImageGeneration, llm::LLMModelType::VideoGeneration,
        llm::LLMModelType::Speech,          llm::LLMModelType::Transcription,
        llm::LLMModelType::Moderation,      llm::LLMModelType::Others,
    };
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            // Plain comparison, not BOOST_CHECK_NE: Boost.Test streams CHECK_NE
            // operands for diagnostics and the enum has no operator<<.
            BOOST_CHECK(all[i] != all[j]);
        }
    }
    // The value is part of the plugin ABI: it must stay a small fixed-width
    // integer, and Conversation (the first implemented type) must be 0 so a
    // zero-initialised value is never a phantom type.
    static_assert(std::is_same_v<std::underlying_type_t<llm::LLMModelType>,
                                 std::uint8_t>);
    BOOST_CHECK_EQUAL(static_cast<std::uint8_t>(llm::LLMModelType::Conversation), 0);
}

// ---------------------------------------------------------------------------
// LLMModel defaults
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(default_converse_throws_unsupported) {
    boost::asio::io_context io;
    bool threw_unsupported = false;
    boost::asio::co_spawn(io,
        [&]() -> boost::asio::awaitable<void> {
            BareModel model{io.get_executor(), nlohmann::json::object()};
            model_io::AgentInputState state;
            try {
                co_await model.converse(state);
            } catch (const llm::LLMUnsupportedOperation&) {
                threw_unsupported = true;
            }
        },
        boost::asio::detached);
    io.run();
    BOOST_CHECK(threw_unsupported);
}

BOOST_AUTO_TEST_CASE(integrate_places_each_item_type) {
    BareModel model{boost::asio::any_io_executor{}, nlohmann::json::object()};
    model_io::AgentInputState state;

    // User input -> a fresh turn.
    model.integrate(state, user_item("hello"));
    BOOST_REQUIRE_EQUAL(state.turns.size(), 1u);
    BOOST_CHECK_EQUAL(state.turns[0].user_input.content.at(0).raw, "hello");
    BOOST_CHECK(state.turns[0].agent_loop_step.empty());

    // Model response -> a new agent step on that same turn.
    model.integrate(state, response_item("thinking..."));
    BOOST_REQUIRE_EQUAL(state.turns.size(), 1u);
    BOOST_REQUIRE_EQUAL(state.turns[0].agent_loop_step.size(), 1u);
    BOOST_CHECK_EQUAL(state.turns[0].agent_loop_step[0].model_response.content.at(0).raw,
                      "thinking...");
    BOOST_CHECK(!state.turns[0].agent_loop_step[0].invoke_returns.has_value());

    // Tool result -> folded into that step's invoke_returns.
    model.integrate(state, result_item("12:00"));
    BOOST_REQUIRE_EQUAL(state.turns[0].agent_loop_step.size(), 1u);
    BOOST_REQUIRE(state.turns[0].agent_loop_step[0].invoke_returns.has_value());
    BOOST_REQUIRE_EQUAL(state.turns[0].agent_loop_step[0].invoke_returns->size(), 1u);
    BOOST_CHECK_EQUAL(
        (*state.turns[0].agent_loop_step[0].invoke_returns)[0].content.at(0).raw,
        "12:00");

    // A second user input starts a SECOND turn; nothing bleeds into the first.
    model.integrate(state, user_item("and now?"));
    BOOST_REQUIRE_EQUAL(state.turns.size(), 2u);
    BOOST_CHECK(state.turns[1].agent_loop_step.empty());
}

BOOST_AUTO_TEST_CASE(integrate_orphans_get_host_structures) {
    BareModel model{boost::asio::any_io_executor{}, nlohmann::json::object()};
    model_io::AgentInputState state;

    // A response before any user turn (a resumed conversation) is hosted in a
    // fresh turn rather than dropped.
    model.integrate(state, response_item("continued"));
    BOOST_REQUIRE_EQUAL(state.turns.size(), 1u);
    BOOST_REQUIRE_EQUAL(state.turns[0].agent_loop_step.size(), 1u);

    // A tool result with no step to belong to creates the host step.
    model_io::AgentInputState fresh;
    model.integrate(fresh, result_item("orphan"));
    BOOST_REQUIRE_EQUAL(fresh.turns.size(), 1u);
    BOOST_REQUIRE_EQUAL(fresh.turns[0].agent_loop_step.size(), 1u);
    BOOST_REQUIRE(fresh.turns[0].agent_loop_step[0].invoke_returns.has_value());
    BOOST_CHECK_EQUAL(fresh.turns[0].agent_loop_step[0].invoke_returns->size(), 1u);
}

BOOST_AUTO_TEST_CASE(integrated_state_stays_serialisable) {
    BareModel model{boost::asio::any_io_executor{}, nlohmann::json::object()};
    model_io::AgentInputState state;
    model.integrate(state, user_item("hello"));
    model.integrate(state, response_item("hi — checking the time"));
    model.integrate(state, result_item("12:00"));
    model.integrate(state, response_item("it is 12:00"));

    // The persistence guarantee: every folded record survives a to_json /
    // from_json roundtrip (model_io's own serializers; the model only has to
    // keep the state well-formed).
    nlohmann::json j = state.turns.back();
    model_io::UserLoopStep back = j.get<model_io::UserLoopStep>();
    BOOST_REQUIRE_EQUAL(back.agent_loop_step.size(), 2u);
    BOOST_CHECK_EQUAL(back.user_input.content.at(0).raw, "hello");
    BOOST_CHECK_EQUAL(back.agent_loop_step[0].model_response.content.at(0).raw,
                      "hi — checking the time");
    BOOST_REQUIRE(back.agent_loop_step[0].invoke_returns.has_value());
    BOOST_CHECK_EQUAL(back.agent_loop_step[1].model_response.content.at(0).raw,
                      "it is 12:00");
}

// ---------------------------------------------------------------------------
// create_model lifecycle (through an in-memory descriptor)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(create_model_runs_build_once_and_release_on_destruction) {
    CountingModel::builds = CountingModel::releases = 0;

    llm::LLMDispatcher dispatcher;
    auto ctx = std::make_shared<FakeContext>();
    ctx->n = "counting";
    dispatcher.add(ctx);

    auto model = dispatcher.create_model("counting", boost::asio::any_io_executor{},
                                         sample_config("counting"));
    BOOST_REQUIRE(model != nullptr);
    BOOST_CHECK(model->model_type() == llm::LLMModelType::Conversation);
    BOOST_CHECK_EQUAL(CountingModel::builds, 1);
    BOOST_CHECK_EQUAL(CountingModel::releases, 0);

    // The config is injected verbatim — the plugin decides how to read it.
    auto* concrete = dynamic_cast<CountingModel*>(model.get());
    BOOST_REQUIRE(concrete != nullptr);
    BOOST_CHECK_EQUAL((*concrete->config().find("provider")), "counting");

    // A second owner: release waits for the LAST reference.
    auto second = model;
    model.reset();
    BOOST_CHECK_EQUAL(CountingModel::releases, 0);
    second.reset();
    BOOST_CHECK_EQUAL(CountingModel::releases, 1);
    BOOST_CHECK_EQUAL(CountingModel::builds, 1);
}

BOOST_AUTO_TEST_CASE(create_model_build_failure_yields_null_and_no_release) {
    FailingBuildModel::builds = FailingBuildModel::releases = 0;

    llm::LLMDispatcher dispatcher;
    auto ctx = std::make_shared<FakeContext>();
    ctx->n = "failing";
    ctx->kind = MintedKind::FailingBuild;
    dispatcher.add(ctx);

    auto model = dispatcher.create_model("failing", boost::asio::any_io_executor{},
                                         nlohmann::json::object());
    BOOST_CHECK(model == nullptr);
    BOOST_CHECK_EQUAL(FailingBuildModel::builds, 1);
    // A failed build() had nothing to tear down: no release() call.
    BOOST_CHECK_EQUAL(FailingBuildModel::releases, 0);
}

// ---------------------------------------------------------------------------
// LLMDispatcher routing + admission gates
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(dispatcher_routes_by_provider_name) {
    llm::LLMDispatcher dispatcher;
    for (const char* provider : {"alpha", "beta"}) {
        auto ctx = std::make_shared<FakeContext>();
        ctx->n = provider;
        dispatcher.add(ctx);
    }

    for (const char* provider : {"alpha", "beta"}) {
        auto model = dispatcher.create_model(provider, boost::asio::any_io_executor{},
                                             sample_config(provider));
        BOOST_REQUIRE(model != nullptr);
        auto* concrete = dynamic_cast<CountingModel*>(model.get());
        BOOST_REQUIRE(concrete != nullptr);
        BOOST_CHECK_EQUAL((*concrete->config().find("provider")), provider);
    }

    // Unknown provider: a null result, not an error.
    BOOST_CHECK(dispatcher.create_model("gamma", boost::asio::any_io_executor{},
                                        nlohmann::json::object()) == nullptr);
}

BOOST_AUTO_TEST_CASE(admission_gates_reject_wrong_abi_and_wrong_type) {
    llm::LLMDispatcher dispatcher;

    // ABI mismatch: stays registered (dispatchable by name) but inert.
    auto stale = std::make_shared<FakeContext>();
    stale->n = "stale";
    stale->abi = llm::LLM_PLUGIN_ABI_VERSION + 1;
    dispatcher.add(stale);
    BOOST_CHECK(!llm::LLMDispatcher::verify_llm_context(stale));
    BOOST_CHECK(dispatcher.create_model("stale", boost::asio::any_io_executor{},
                                        nlohmann::json::object()) == nullptr);

    // Right ABI but not an llm descriptor at all.
    auto alien = std::make_shared<NonLlmContext>();
    dispatcher.add(alien);
    BOOST_CHECK(!llm::LLMDispatcher::verify_llm_context(alien));
    BOOST_CHECK(dispatcher.create_model("alien", boost::asio::any_io_executor{},
                                        nlohmann::json::object()) == nullptr);

    // The happy path for contrast.
    auto good = std::make_shared<FakeContext>();
    good->n = "good";
    dispatcher.add(good);
    BOOST_CHECK(llm::LLMDispatcher::verify_llm_context(good));
}

BOOST_AUTO_TEST_CASE(descriptor_type_defaults_to_conversation) {
    FakeContext ctx;
    ctx.n = "typed";
    BOOST_CHECK(ctx.get_type() == llm::LLMModelType::Conversation);
    BOOST_CHECK(ctx.warm());   // in-memory path: nothing to resolve
    BOOST_CHECK(ctx.warm());   // idempotent
}

BOOST_AUTO_TEST_SUITE_END()
