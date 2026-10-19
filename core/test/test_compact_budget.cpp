#define BOOST_TEST_MODULE CoreCompactBudget
#include <boost/test/unit_test.hpp>
#include "core/compact.hpp"
#include "core/protocol.hpp"

namespace fs = std::filesystem;
using Json = nlohmann::json;

BOOST_AUTO_TEST_CASE(preview_accounts_for_fixed_overhead_and_one_shared_slack) {
    model_io::AgentInputState state;
    state.system_prompt.add_section("base", "Instructions", std::string(40 * 1024, 'P'));
    state.system_prompt.add_section("memory.runtime", "Memory", std::string(80 * 1024, 'M'));
    state.tools.push_back({});
    const auto file = fs::path("/tmp") / std::string(150, 'D') / "memory.md";
    const auto plan = core::plan_compact(state, file);
    BOOST_TEST(plan.fixed_bytes + plan.summary_bytes == core::compact_context_max_bytes + core::compact_context_slack_bytes);
    BOOST_TEST(plan.fixed_bytes == plan.prompt.render().markdown.size() + Json(state.tools).dump().size());
    BOOST_CHECK(plan.prompt.render().markdown.find(std::string(100, 'M')) == std::string::npos);
    const auto preview = core::compact_prompt(plan, std::string(2 * plan.summary_bytes, 'S'));
    BOOST_TEST(preview.truncated);
    BOOST_CHECK(preview.summary.size() <= plan.summary_bytes);
    BOOST_CHECK(preview.summary.find(file.string()) != std::string::npos);
    BOOST_CHECK(preview.summary.find("Summary truncated") != std::string::npos);
    model_io::AgentInputState replacement;
    replacement.tools = state.tools;
    replacement.system_prompt = preview.prompt;
    BOOST_CHECK(core::compact_context_bytes(replacement) <= plan.hard_target);
}

BOOST_AUTO_TEST_CASE(preview_preserves_utf8_and_prefers_nearby_newline) {
    model_io::AgentInputState state;
    const auto plan = core::plan_compact(state, "/memory.md");
    std::string text(plan.body_bytes - 2, 'a');
    text += "🌍trailing";
    auto preview = core::compact_prompt(plan, text);
    BOOST_TEST(preview.injected_bytes == plan.body_bytes - 2);
    BOOST_CHECK_NO_THROW(Json(preview.summary).dump());
    text = std::string(plan.body_bytes - 20, 'a') + "\n" + std::string(100, 'b');
    preview = core::compact_prompt(plan, text);
    BOOST_TEST(preview.injected_bytes == plan.body_bytes - 19);
    BOOST_CHECK(preview.summary.size() <= core::compact_summary_max_bytes);
}

BOOST_AUTO_TEST_CASE(small_context_uses_relaxed_reduction_target_and_long_path_fails_preflight) {
    model_io::AgentInputState state;
    state.system_prompt.add_section("base", "", "Instructions");
    const auto before = core::compact_context_bytes(state);
    const auto plan = core::plan_compact(state, "/memory.md");
    BOOST_TEST(plan.hard_target == before - (before / 10 + (before % 10 != 0)) + core::compact_context_slack_bytes);
    const auto short_summary = core::compact_prompt(plan, "Keep the user's goal.");
    BOOST_TEST(!short_summary.truncated);
    BOOST_CHECK(short_summary.summary.find("Keep the user's goal.") != std::string::npos);
    BOOST_CHECK_THROW(core::plan_compact(state, fs::path("/") / std::string(10000, 'p')), std::runtime_error);
    state.system_prompt.add_section("huge", "", std::string(70 * 1024, 'P'));
    BOOST_CHECK_EXCEPTION(core::plan_compact(state, "/memory.md"), std::runtime_error,
        [](const auto& error) { return std::string(error.what()).find("fixed_bytes=") != std::string::npos; });
}

BOOST_AUTO_TEST_CASE(protects_explicit_absolute_references_in_retained_state) {
    model_io::AgentInputState state;
    const fs::path root = "/tmp/memory-目标\"\\";
    state.system_prompt.add_section("memory.runtime", "Memory", (root / "first/state.json").string());
    state.extras = Json{{"retrieval", Json::array({(root / "second/state.json").string(),
        (root / "first/state.json").string(), "relative/third/state.json"})}};
    model_io::UserLoopStep turn;
    turn.user_input.content.push_back({});
    turn.user_input.content.back().raw = "Look at " + (root / "third/state.json").string();
    state.turns.push_back(std::move(turn));
    const auto references = core::compact_archive_references(state, root);
    BOOST_TEST(references.size() == 3u);
    for (const auto& name : {"first", "second", "third"}) {
        BOOST_CHECK(std::find(references.begin(), references.end(), root / name) != references.end());
    }
}
