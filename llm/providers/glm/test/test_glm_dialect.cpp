/**
 * @file test_glm_dialect.cpp
 * @brief Unit tests for llm/glm/dialect.hpp — GLM's policy matrix around the
 *        neutral chat-completions wire.
 *
 * Every provider deviation is pinned: the endpoint defaults (base URL stops at
 * the version prefix so a Coding Plan account only overrides base_url), the
 * catalogue path, the always-on thinking policy of the GLM-5.3 family, the
 * medium/xhigh effort aliases against the same family, the refusal of
 * none/minimal, the ignored-and-stripped controls, the tool_choice vocabulary,
 * and the finish_reason mapping that turns a moderation or context-overflow
 * end into a verdict the shared reader already knows. All offline: the dialect
 * is a pure JSON→JSON transformation.
 */

#define BOOST_TEST_MODULE glm_dialect
#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "dataclass/endpoint_config.hpp"
#include "llm/glm/dialect.hpp"

using llm::glm::GlmDialect;

namespace {

/// transform_request over an initial body, returning the result.
nlohmann::json transformed(nlohmann::json body) {
    GlmDialect dialect;
    dialect.transform_request(body);
    return body;
}

/// The diagnostic message of the refusal @p body provokes.
std::string refusal(nlohmann::json body) {
    try {
        llm::glm::prepare_generation(body);
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

} // namespace

// --- endpoint defaults ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(default_endpoint_points_at_glm_with_bearer) {
    const model_io::ModelEndpoint endpoint = GlmDialect{}.default_endpoint();
    BOOST_CHECK_EQUAL(endpoint.base_url,
                      "https://open.bigmodel.cn/api/paas/v4");
    BOOST_CHECK_EQUAL(endpoint.request_path, "/chat/completions");
    BOOST_CHECK(endpoint.auth.scheme == model_io::AuthScheme::Bearer);
    BOOST_CHECK_EQUAL(endpoint.user_agent, "simplex-cpp/glm");
}

// The catalogue query provider_info() runs resolves against the same prefix,
// so a Coding Plan base_url re-points both the exchange and the catalogue.
BOOST_AUTO_TEST_CASE(models_path_is_relative_to_the_version_prefix) {
    BOOST_CHECK_EQUAL(GlmDialect{}.models_path(), "/models");
}

BOOST_AUTO_TEST_CASE(provider_name_identifies_glm) {
    BOOST_CHECK_EQUAL(GlmDialect{}.provider_name(), "glm");
}

// Interleaved thinking with tools requires the reasoning to travel next to the
// tool results, so replay is on for every GLM model.
BOOST_AUTO_TEST_CASE(reasoning_is_replayed) {
    BOOST_CHECK(GlmDialect{}.replay_assistant_reasoning());
}

// --- always-on thinking --------------------------------------------------------

BOOST_AUTO_TEST_CASE(disabling_thinking_is_refused_on_the_53_family) {
    for (const char* model : {"glm-5.3", "glm-5.3-flash", "glm-5.3-flashx",
                              "glm-5.3-air"}) {
        BOOST_CHECK_THROW(
            transformed({{"model", model},
                         {"thinking", {{"type", "disabled"}}}}),
            std::invalid_argument);
    }
}

BOOST_AUTO_TEST_CASE(disabling_thinking_passes_through_on_glm_47) {
    for (const char* model : {"glm-4.7", "glm-4.7-flash"}) {
        const auto body = transformed({{"model", model},
            {"thinking", {{"type", "disabled"}, {"clear_thinking", false}}}});
        BOOST_CHECK_EQUAL(body["thinking"]["type"], "disabled");
        BOOST_CHECK_EQUAL(body["thinking"]["clear_thinking"], false);
    }
}

// A model the dialect has no evidence about keeps the permissive path: the
// native object is forwarded for the service to judge.
BOOST_AUTO_TEST_CASE(disabling_thinking_passes_through_on_other_models) {
    const auto body = transformed({{"model", "glm-4.6"},
                                   {"thinking", {{"type", "disabled"}}}});
    BOOST_CHECK_EQUAL(body["thinking"]["type"], "disabled");
}

BOOST_AUTO_TEST_CASE(enabled_thinking_and_clear_thinking_pass_through) {
    const auto body = transformed({
        {"model", "glm-5.3-flash"},
        {"thinking", {{"type", "enabled"}, {"clear_thinking", false}}},
    });
    BOOST_CHECK_EQUAL(body["thinking"]["type"], "enabled");
    BOOST_CHECK_EQUAL(body["thinking"]["clear_thinking"], false);
}

BOOST_AUTO_TEST_CASE(thinking_must_be_an_object_with_typed_members) {
    BOOST_CHECK_THROW(transformed({{"model", "glm-5.3-flash"},
                                   {"thinking", "enabled"}}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(
        transformed({{"model", "glm-5.3-flash"},
                     {"thinking", {{"type", 1}}}}),
        std::invalid_argument);
    BOOST_CHECK_THROW(
        transformed({{"model", "glm-5.3-flash"},
                     {"thinking", {{"clear_thinking", "no"}}}}),
        std::invalid_argument);
}

// --- effort vocabulary ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(document_efforts_pass_through_on_the_53_family) {
    for (const char* effort : {"low", "high", "max"}) {
        const auto body = transformed({{"model", "glm-5.3-flash"},
                                       {"reasoning_effort", effort}});
        BOOST_CHECK_EQUAL(body["reasoning_effort"], effort);
    }
}

// The family's predecessors document these two as aliases of the values it
// does take; translating keeps a portable configuration working.
BOOST_AUTO_TEST_CASE(medium_and_xhigh_are_translated_on_the_53_family) {
    BOOST_CHECK_EQUAL(
        transformed({{"model", "glm-5.3-flash"}, {"reasoning_effort", "medium"}})
            ["reasoning_effort"],
        "high");
    BOOST_CHECK_EQUAL(
        transformed({{"model", "glm-5.3"}, {"reasoning_effort", "xhigh"}})
            ["reasoning_effort"],
        "max");
}

// none/minimal mean "stop thinking" on the family's predecessors; the 5.3
// family cannot, so landing on `low` would answer a different question.
BOOST_AUTO_TEST_CASE(none_and_minimal_are_refused_on_the_53_family) {
    for (const char* effort : {"none", "minimal"}) {
        BOOST_CHECK_THROW(
            transformed({{"model", "glm-5.3-flash"},
                         {"reasoning_effort", effort}}),
            std::invalid_argument);
    }
    BOOST_CHECK_THROW(
        transformed({{"model", "glm-5.3-flash"},
                     {"reasoning_effort", "unheard-of"}}),
        std::invalid_argument);
}

// Any other family keeps the neutral pass-through: the service owns the
// verdict for a model the dialect has not been told about.
BOOST_AUTO_TEST_CASE(effort_passes_through_unchanged_on_other_models) {
    const auto body = transformed({{"model", "glm-4.6"},
                                   {"reasoning_effort", "medium"}});
    BOOST_CHECK_EQUAL(body["reasoning_effort"], "medium");
}

BOOST_AUTO_TEST_CASE(effort_must_be_a_nonempty_string) {
    BOOST_CHECK_THROW(
        transformed({{"model", "glm-5.3-flash"}, {"reasoning_effort", 3}}),
        std::invalid_argument);
    BOOST_CHECK_THROW(
        transformed({{"model", "glm-5.3-flash"}, {"reasoning_effort", ""}}),
        std::invalid_argument);
}

// The shared host config envelope is the Responses spelling; GLM takes the
// top-level field and would reject the unknown member.
BOOST_AUTO_TEST_CASE(reasoning_envelope_becomes_reasoning_effort) {
    const auto body = transformed({{"model", "glm-5.3-flash"},
                                   {"reasoning", {{"effort", "medium"}}}});
    BOOST_CHECK_EQUAL(body["reasoning_effort"], "high");
    BOOST_CHECK(!body.contains("reasoning"));
}

BOOST_AUTO_TEST_CASE(explicit_effort_wins_over_the_envelope) {
    const auto body = transformed({{"model", "glm-5.3-flash"},
                                   {"reasoning", {{"effort", "medium"}}},
                                   {"reasoning_effort", "low"}});
    BOOST_CHECK_EQUAL(body["reasoning_effort"], "low");
}

// merge_patch has no erase, so a typed null is how a patch clears a control.
BOOST_AUTO_TEST_CASE(null_effort_clears_the_control) {
    const auto body = transformed({{"model", "glm-5.3-flash"},
                                   {"reasoning_effort", nullptr}});
    BOOST_CHECK(!body.contains("reasoning_effort"));
}

BOOST_AUTO_TEST_CASE(null_envelope_effort_does_not_become_a_value) {
    const auto body = transformed({{"model", "glm-5.3-flash"},
                                   {"reasoning", {{"effort", nullptr}}}});
    BOOST_CHECK(!body.contains("reasoning_effort"));
    BOOST_CHECK(!body.contains("reasoning"));
}

// --- request hygiene -----------------------------------------------------------

// Accepted by the service but ignored (verified 200 on 2026-10-30); stripping
// them keeps a stale configuration from looking like a working control.
BOOST_AUTO_TEST_CASE(ignored_controls_are_stripped) {
    const auto body = transformed({
        {"model", "glm-5.3-flash"},
        {"frequency_penalty", 0.5},
        {"presence_penalty", 0.5},
        {"thinking_budget", 512},
    });
    BOOST_CHECK(!body.contains("frequency_penalty"));
    BOOST_CHECK(!body.contains("presence_penalty"));
    BOOST_CHECK(!body.contains("thinking_budget"));
}

// `n` is not part of the documented request and the builder's fixed n = 1
// must not reach this wire.
BOOST_AUTO_TEST_CASE(n_is_stripped) {
    const auto body = transformed({{"model", "glm-5.3-flash"}, {"n", 1}});
    BOOST_CHECK(!body.contains("n"));
}

BOOST_AUTO_TEST_CASE(tool_choice_auto_passes_and_other_values_are_refused) {
    BOOST_CHECK_EQUAL(
        transformed({{"model", "glm-5.3-flash"}, {"tool_choice", "auto"}})
            ["tool_choice"],
        "auto");
    for (const auto& choice : {nlohmann::json("required"),
                               nlohmann::json("none"),
                               nlohmann::json{{"type", "function"}}}) {
        BOOST_CHECK_THROW(
            transformed({{"model", "glm-5.3-flash"}, {"tool_choice", choice}}),
            std::invalid_argument);
    }
}

BOOST_AUTO_TEST_CASE(tool_stream_must_be_boolean) {
    BOOST_CHECK_EQUAL(
        transformed({{"model", "glm-5.3-flash"}, {"tool_stream", true}})
            ["tool_stream"],
        true);
    BOOST_CHECK_THROW(
        transformed({{"model", "glm-5.3-flash"}, {"tool_stream", "on"}}),
        std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(model_is_required) {
    BOOST_CHECK_THROW(transformed({{"thinking", {{"type", "enabled"}}}}),
                      std::invalid_argument);
    BOOST_CHECK_THROW(transformed({{"model", ""}}), std::invalid_argument);
    BOOST_CHECK_THROW(transformed({{"model", 7}}), std::invalid_argument);
}

// A diagnostic names the field (and at most the configured model), never the
// caller's value.
BOOST_AUTO_TEST_CASE(diagnostics_do_not_echo_caller_values) {
    const std::string disabled =
        refusal({{"model", "glm-5.3-flash"},
                 {"thinking", {{"type", "disabled"}}}});
    BOOST_CHECK(disabled.find("thinking.type") != std::string::npos);
    BOOST_CHECK(disabled.find("reasoning_effort") != std::string::npos);

    const std::string effort =
        refusal({{"model", "glm-5.3-flash"},
                 {"reasoning_effort", "minimal"}});
    BOOST_CHECK(effort.find("reasoning_effort") != std::string::npos);
    BOOST_CHECK(effort.find("minimal") == std::string::npos);

    const std::string choice =
        refusal({{"model", "glm-5.3-flash"}, {"tool_choice", "required"}});
    BOOST_CHECK(choice.find("tool_choice") != std::string::npos);
    BOOST_CHECK(choice.find("required") == std::string::npos);
}

// --- response vocabulary -------------------------------------------------------

namespace {

nlohmann::json normalized(nlohmann::json chunk) {
    GlmDialect dialect;
    return dialect.normalize_chunk(std::move(chunk));
}

nlohmann::json chunk_with(const char* finish_reason) {
    return {
        {"choices", {{{"index", 0},
                      {"finish_reason", finish_reason},
                      {"delta", {{"content", ""}}}}}},
    };
}

} // namespace

// GLM reports a moderation stop as `sensitive`; the shared reader only knows
// content_filter, and without the mapping the exchange would end as a generic
// failure.
BOOST_AUTO_TEST_CASE(sensitive_maps_to_content_filter) {
    BOOST_CHECK_EQUAL(normalized(chunk_with("sensitive"))["choices"][0]
                          ["finish_reason"],
                      "content_filter");
}

// Context overflow is a length verdict, not a protocol failure.
BOOST_AUTO_TEST_CASE(context_overflow_maps_to_length) {
    BOOST_CHECK_EQUAL(
        normalized(chunk_with("model_context_window_exceeded"))["choices"][0]
            ["finish_reason"],
        "length");
}

BOOST_AUTO_TEST_CASE(document_finish_reasons_are_untouched) {
    for (const char* reason : {"stop", "tool_calls", "length", "content_filter",
                               "sensitive_but_not_really"}) {
        BOOST_CHECK_EQUAL(
            normalized(chunk_with(reason))["choices"][0]["finish_reason"],
            reason);
    }
}

// A model-side fault must stay a failure: mapping it would report a completed
// exchange that never completed.
BOOST_AUTO_TEST_CASE(network_error_stays_a_failure) {
    BOOST_CHECK_EQUAL(normalized(chunk_with("network_error"))["choices"][0]
                          ["finish_reason"],
                      "network_error");
}

BOOST_AUTO_TEST_CASE(normalization_leaves_non_choices_payloads_alone) {
    const nlohmann::json usage_only{
        {"usage", {{"prompt_tokens", 1}, {"completion_tokens", 2}}}};
    BOOST_CHECK(normalized(usage_only) == usage_only);
    const nlohmann::json text = "not an object";
    BOOST_CHECK(normalized(text) == text);
}
