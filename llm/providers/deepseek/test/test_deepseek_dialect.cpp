/**
 * @file test_deepseek_dialect.cpp
 * @brief Unit tests for llm/deepseek/dialect.hpp — DeepSeek's policy matrix
 *        around the neutral chat-completions wire.
 *
 * Every provider deviation is pinned: the endpoint defaults, the
 * thinking-mode toggle (server default enabled, native object passthrough,
 * none/minimal disable), the effort vocabulary pass-through (the server's
 * live 2026-08 vocabulary is none|minimal|low|medium|high|xhigh|max and it
 * rejects out-of-vocabulary values itself), the undocumented/deprecated
 * parameter stripping, and the prompt_cache_hit_tokens → cached_tokens
 * usage bridge. All offline: the dialect is a pure JSON→JSON
 * transformation.
 */

#define BOOST_TEST_MODULE deepseek_dialect
#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"
#include "llm/deepseek/dialect.hpp"

using llm::deepseek::DeepSeekDialect;

namespace {

/// transform_request over an initial body, returning the result.
nlohmann::json transformed(nlohmann::json body) {
    DeepSeekDialect dialect;
    dialect.transform_request(body);
    return body;
}

} // namespace

// --- endpoint defaults ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(default_endpoint_points_at_deepseek_with_bearer) {
    const model_io::ModelEndpoint endpoint =
        DeepSeekDialect{}.default_endpoint();
    BOOST_CHECK_EQUAL(endpoint.base_url, "https://api.deepseek.com");
    BOOST_CHECK_EQUAL(endpoint.request_path, "/chat/completions");
    BOOST_CHECK(endpoint.auth.scheme == model_io::AuthScheme::Bearer);
    BOOST_CHECK_EQUAL(endpoint.user_agent, "simplex-cpp/deepseek");
}

// The catalogue query provider_info() runs — same host, no /v1 prefix
// (https://api.deepseek.com/models), unlike the base dialect's default.
BOOST_AUTO_TEST_CASE(models_path_has_no_v1_prefix) {
    BOOST_CHECK_EQUAL(DeepSeekDialect{}.models_path(), "/models");
    BOOST_CHECK_EQUAL(
        llm::chat_completions::ChatCompletionsDialect{}.models_path(),
        "/v1/models");
}

// The account-balance companion provider_info() attaches after the
// catalogue — same host, same auth (https://api.deepseek.com/user/balance).
// The base default is empty: no companion, bare models array.
BOOST_AUTO_TEST_CASE(balance_path_is_the_deepseek_account_endpoint) {
    BOOST_CHECK_EQUAL(DeepSeekDialect{}.balance_path(), "/user/balance");
    BOOST_CHECK_EQUAL(
        llm::chat_completions::ChatCompletionsDialect{}.balance_path(), "");
}

// --- thinking-mode request policy ----------------------------------------------

BOOST_AUTO_TEST_CASE(missing_effort_defaults_thinking_to_enabled) {
    const auto body = transformed({{"model", "deepseek-v4-flash"}});
    BOOST_CHECK_EQUAL(body["thinking"]["type"], "enabled");
    BOOST_CHECK(!body.contains("reasoning_effort"));
}

BOOST_AUTO_TEST_CASE(native_thinking_object_passes_through) {
    const auto body = transformed({
        {"model", "deepseek-v4-flash"},
        {"thinking", {{"type", "disabled"}, {"budget_tokens", 512}}},
    });
    BOOST_CHECK_EQUAL(body["thinking"]["type"], "disabled");
    BOOST_CHECK_EQUAL(body["thinking"]["budget_tokens"], 512);

    // A non-object thinking value is not native intent: the dialect
    // replaces it with a valid enabled object.
    const auto repaired = transformed({
        {"model", "deepseek-v4-flash"},
        {"thinking", "yes"},
    });
    BOOST_CHECK_EQUAL(repaired["thinking"]["type"], "enabled");
}

BOOST_AUTO_TEST_CASE(effort_none_and_minimal_disable_thinking_and_erase_effort) {
    for (const char* effort : {"none", "minimal"}) {
        const auto body = transformed({
            {"model", "deepseek-v4-flash"},
            {"reasoning_effort", effort},
        });
        BOOST_CHECK_EQUAL(body["thinking"]["type"], "disabled");
        BOOST_CHECK(!body.contains("reasoning_effort"));
    }
}

BOOST_AUTO_TEST_CASE(effort_vocabulary_passes_through_verbatim) {
    // Live 2026-08: the server accepts none|minimal|low|medium|high|xhigh|max
    // and answers 400 with that enumeration for anything else — so the
    // dialect must NOT reshape the caller's effort. (An earlier clamp
    // low/medium→high, xhigh→max mirrored docs that have since been
    // restructured; it silently bought more thinking than asked for.)
    for (const char* effort : {"low", "medium", "high", "xhigh", "max"}) {
        const auto body = transformed({
            {"model", "deepseek-v4-flash"},
            {"reasoning_effort", effort},
        });
        BOOST_CHECK_EQUAL(body["thinking"]["type"], "enabled");
        BOOST_CHECK_EQUAL(body["reasoning_effort"], effort);
    }
    // An out-of-vocabulary effort is likewise passed through untouched: the
    // server's 400 ("unknown variant ... expected one of none, minimal, low,
    // medium, high, xhigh, max") is a better failure than a silent rewrite.
    const auto bogus = transformed({
        {"model", "deepseek-v4-flash"},
        {"reasoning_effort", "turbo"},
    });
    BOOST_CHECK_EQUAL(bogus["thinking"]["type"], "enabled");
    BOOST_CHECK_EQUAL(bogus["reasoning_effort"], "turbo");
}

BOOST_AUTO_TEST_CASE(undocumented_and_deprecated_parameters_are_stripped) {
    const auto body = transformed({
        {"model", "deepseek-v4-flash"},
        {"n", 3},
        {"frequency_penalty", 0.5},
        {"presence_penalty", 0.5},
        {"temperature", 0.7},
        {"top_p", 0.9},
        {"max_tokens", 2048},
        {"stop", nlohmann::json::array({"END"})},
        {"response_format", {{"type", "json_object"}}},
    });
    BOOST_CHECK(!body.contains("n"));
    BOOST_CHECK(!body.contains("frequency_penalty"));
    BOOST_CHECK(!body.contains("presence_penalty"));
    BOOST_CHECK_EQUAL(body["temperature"], 0.7);
    BOOST_CHECK_EQUAL(body["top_p"], 0.9);
    BOOST_CHECK_EQUAL(body["max_tokens"], 2048);
    BOOST_CHECK_EQUAL(body["stop"][0], "END");
    BOOST_CHECK_EQUAL(body["response_format"]["type"], "json_object");
}

// --- usage normalisation -------------------------------------------------------

BOOST_AUTO_TEST_CASE(chunks_without_usage_pass_through_unchanged) {
    DeepSeekDialect dialect;
    const nlohmann::json chunk = {
        {"id", "chatcmpl_1"},
        {"object", "chat.completion.chunk"},
        {"choices", nlohmann::json::array({
            {"index", 0},
            {"delta", {{"content", "hello"}}},
            {"finish_reason", nullptr},
        })},
    };
    BOOST_CHECK(dialect.normalize_chunk(chunk) == chunk);
}

BOOST_AUTO_TEST_CASE(cache_hit_tokens_are_normalised_into_cached_tokens) {
    DeepSeekDialect dialect;
    const auto chunk = dialect.normalize_chunk({
        {"id", "chatcmpl_2"},
        {"choices", nlohmann::json::array()},
        {"usage", {
            {"prompt_tokens", 20},
            {"completion_tokens", 5},
            {"total_tokens", 25},
            {"prompt_cache_hit_tokens", 14},
            {"prompt_cache_miss_tokens", 6},
        }},
    });
    const auto& usage = chunk.at("usage");
    BOOST_CHECK_EQUAL(
        usage.at("prompt_tokens_details").at("cached_tokens"), 14);
    // The native spelling stays: extras consumers still see it.
    BOOST_CHECK_EQUAL(usage.at("prompt_cache_hit_tokens"), 14);
    BOOST_CHECK_EQUAL(usage.at("prompt_cache_miss_tokens"), 6);
}

BOOST_AUTO_TEST_CASE(existing_cached_tokens_win_over_cache_hit_tokens) {
    DeepSeekDialect dialect;
    const auto chunk = dialect.normalize_chunk({
        {"choices", nlohmann::json::array()},
        {"usage", {
            {"prompt_cache_hit_tokens", 4},
            {"prompt_tokens_details", {{"cached_tokens", 9}}},
        }},
    });
    BOOST_CHECK_EQUAL(
        chunk.at("usage").at("prompt_tokens_details").at("cached_tokens"), 9);
}

// --- reasoning replay ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(assistant_reasoning_replay_is_opted_in) {
    BOOST_CHECK(DeepSeekDialect{}.replay_assistant_reasoning());
}
