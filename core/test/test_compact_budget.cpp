#define BOOST_TEST_MODULE CoreCompactBudget
#include <boost/test/unit_test.hpp>
#include "core/compact.hpp"
#include "core/protocol.hpp"

namespace fs = std::filesystem;
using Json = nlohmann::json;

BOOST_AUTO_TEST_CASE(measures_fixed_overhead_without_old_memory_or_turns) {
    model_io::AgentInputState state;
    state.system_prompt.add_section("base", "Instructions", std::string(40 * 1024, 'P'));
    state.system_prompt.add_section("memory.runtime", "Memory", std::string(80 * 1024, 'M'));
    state.tools.push_back({});
    model_io::UserLoopStep turn;
    turn.user_input.content.push_back({});
    turn.user_input.content.back().raw = std::string(100 * 1024, 'H');
    state.turns.push_back(std::move(turn));
    const auto directory = fs::path("/tmp") / std::string(150, 'D');
    const auto plan = core::plan_compact(state, directory, directory / "archive/state.md");
    BOOST_CHECK(plan.summary_bytes < core::compact_summary_max_bytes);
    BOOST_TEST(plan.fixed_bytes + plan.summary_bytes == core::compact_context_max_bytes);
    BOOST_TEST(plan.fixed_bytes == plan.prompt.render().markdown.size() + Json(state.tools).dump().size());
    BOOST_CHECK(plan.prompt.render().markdown.find(std::string(100, 'M')) == std::string::npos);
    BOOST_CHECK(plan.prompt.render().markdown.find(directory.string()) != std::string::npos);
    const auto short_plan = core::plan_compact(state, "/m", "/m/a/state.md");
    BOOST_CHECK(short_plan.summary_bytes > plan.summary_bytes);
    BOOST_CHECK_THROW(core::compact_prompt(plan, std::string(plan.summary_bytes + 1, 'S')), std::runtime_error);
    std::string summary = "目标🌍\"\\";
    summary.append(plan.summary_bytes - summary.size(), 'S');
    model_io::AgentInputState replacement;
    replacement.tools = state.tools;
    replacement.system_prompt = core::compact_prompt(plan, summary);
    BOOST_CHECK(core::compact_context_bytes(replacement) <= core::compact_context_max_bytes);
    BOOST_CHECK(replacement.system_prompt.render().markdown.find(summary) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(byte_allowance_is_independent_of_provider_token_window) {
    model_io::AgentInputState state;
    state.system_prompt.add_section("base", "", "Ordinary instructions");
    std::size_t allowance = 0;
    for (const auto tokens : {1024, 32768, 1000000}) {
        state.extras = Json{{"external_status", {{"context_statistic", {{"context_window_tokens", tokens}}}}}};
        const auto plan = core::plan_compact(state, "/memory", "/memory/archive/state.md");
        BOOST_TEST(plan.summary_bytes == core::compact_summary_max_bytes);
        if (allowance) BOOST_TEST(plan.summary_bytes == allowance);
        allowance = plan.summary_bytes;
    }
    state.system_prompt.add_section("huge", "", std::string(core::compact_context_max_bytes, 'P'));
    BOOST_CHECK_EXCEPTION(core::plan_compact(state, "/memory", "/memory/archive/state.md"),
        std::runtime_error, [](const auto& error) {
            const std::string text = error.what();
            return text.find("fixed_bytes=") != std::string::npos
                && text.find("budget_bytes=65536") != std::string::npos
                && text.find("shorten") != std::string::npos;
        });
}

BOOST_AUTO_TEST_CASE(protects_explicit_absolute_references_in_retained_state) {
    model_io::AgentInputState state;
    const fs::path root = "/tmp/memory-目标\"\\";
    state.system_prompt.add_section("memory.runtime", "Memory", (root / "first/state.md").string());
    state.extras = Json{{"retrieval", Json::array({(root / "second/state.md").string(),
        (root / "first/state.md").string(), "relative/third/state.md"})}};
    model_io::UserLoopStep turn;
    turn.user_input.content.push_back({});
    turn.user_input.content.back().raw = "Look at " + (root / "third/state.md").string();
    state.turns.push_back(std::move(turn));
    const auto references = core::compact_archive_references(state, root);
    BOOST_TEST(references.size() == 3u);
    for (const auto& name : {"first", "second", "third"}) {
        BOOST_CHECK(std::find(references.begin(), references.end(), root / name) != references.end());
    }
}
