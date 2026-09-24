#define BOOST_TEST_MODULE StatePersistence
#include <boost/test/unit_test.hpp>

#include "load/persistence.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

/** Give each test a private disk tree without modifying the working directory. */
struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_persistence_test_" + std::to_string(::getpid()));

    Scratch() {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    void write(std::string_view name, std::string_view value) const {
        std::ofstream output(root / name);
        output << value;
        BOOST_REQUIRE(output.good());
    }

    std::string read(std::string_view name) const {
        std::ifstream input(root / name);
        BOOST_REQUIRE(input.good());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void no_temporary_files() const {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            BOOST_TEST(!entry.path().filename().string().starts_with(".simplex-write-"));
        }
    }
};

/** Include recovery records and all major message representations in round trips. */
model_io::AgentInputState example_state() {
    model_io::AgentInputState state;
    state.meta.session_id = "session-42";
    state.meta.created_at = "2026-09-24T00:00:00Z";
    state.meta.updated_at = "2026-09-24T01:00:00Z";
    state.meta.error = Json{{"message", "test diagnostic"}};
    state.system_prompt.add_section("system", "Instructions", "Be precise.");
    model_io::Invocable tool;
    tool.name = "probe";
    tool.description = "Inspect a local value.";
    tool.argument_schema = {{"type", "object"}};
    state.tools.push_back(tool);

    model_io::InvokeQuery query;
    query.id = "call-1";
    query.name = "probe";
    query.arguments = {{"payload", "test"}};
    query.extras = Json{{"trace", 7}};
    model_io::InvokeReturn result;
    result.query = query;
    result.output.raw = "tool output";
    result.extras = Json{{"exit_code", 0}};

    model_io::UserLoopStep turn;
    turn.user_input.role = "user";
    turn.user_input.content.push_back({model_io::ContentType::Text, "hello", {}});
    turn.extras = Json{{"request_id", "r1"}};
    model_io::AgentLoopStep step;
    step.commit_sequence = 7;
    step.retain_priority = model_io::RetainPriority::Pinned;
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.content.push_back({model_io::ContentType::Text, "answer", {}});
    step.model_response.reasoning = model_io::Content{model_io::ContentType::Text, "reasoning", {}};
    step.model_response.action_status = model_io::Content{model_io::ContentType::Text, "working", {}};
    step.model_response.invokes = {query};
    step.model_response.cost = model_io::TokenCost{100, 20, 40};
    step.model_response.extras = Json{{"provider_id", "p1"}};
    model_io::MessageItem returned;
    returned.type = model_io::MessageItemType::InvokeReturn;
    returned.role = "tool";
    returned.content.push_back(result.output);
    returned.invoke_return = result;
    step.invoke_returns = {returned};
    step.extras = Json{{"complete", true}};
    turn.agent_loop_step.push_back(step);
    state.turns.push_back(turn);
    state.loop.emplace();
    state.loop->phase = model_io::LoopPhase::Projection;
    state.loop->committed_response_sequence = 7;
    state.loop->completed_exchanges = 1;
    state.loop->pending_results.push_back(result);
    state.extras = Json{{"external_status", {{"test", {{"count", 7}}}}}};
    return state;
}

} // namespace

BOOST_AUTO_TEST_CASE(json_roundtrip_preserves_full_state_and_replaces_existing_snapshot) {
    Scratch scratch;
    auto state = example_state();
    state.extras->operator[]("large") = std::string(20000, 'x');
    const Json before = state;
    load::save_state(scratch.root / "nested/state.json", state);
    BOOST_CHECK(Json(load::load_state(scratch.root / "nested/state.json")) == before);
    BOOST_CHECK(Json(state) == before);
    state.meta.session_id = "replacement";
    load::save_state(scratch.root / "nested/state.json", state);
    BOOST_CHECK(Json(load::load_state(scratch.root / "nested/state.json")) == Json(state));
    const auto permissions = fs::status(scratch.root / "nested/state.json").permissions();
    BOOST_CHECK((permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    scratch.no_temporary_files();
}

BOOST_AUTO_TEST_CASE(json_uses_dataclass_compatibility_without_treating_invalid_files_as_empty) {
    Scratch scratch;
    Json document = example_state();
    document["future"] = true;
    document.erase("loop");
    // Legacy content was a single Content object rather than an array.
    document["turns"][0]["user_input"]["content"] = {
        {"type", "text"}, {"raw", "legacy"}
    };
    scratch.write("state.json", document.dump());
    const auto state = load::load_state(scratch.root / "state.json");
    BOOST_TEST(!state.loop.has_value());
    BOOST_TEST(state.turns[0].user_input.content[0].raw == "legacy");

    for (const auto& bad : {"", "{", "null", "[]", "{}", "{} {}", "# Markdown"}) {
        scratch.write("bad.json", bad);
        BOOST_CHECK_THROW((void)load::load_state(scratch.root / "bad.json"), load::PersistenceError);
    }
    document = example_state();
    document["loop"]["phase"] = "unknown";
    scratch.write("bad.json", document.dump());
    BOOST_CHECK_THROW((void)load::load_state(scratch.root / "bad.json"), load::PersistenceError);
    for (const char* pointer : {"/turns/0", "/system_prompt/sections", "/tools/0",
                                "/turns/0/agent_loop_step/0/model_response/content/0"}) {
        document = example_state();
        document[Json::json_pointer(pointer)] = 42;
        scratch.write("bad.json", document.dump());
        BOOST_CHECK_THROW((void)load::load_state(scratch.root / "bad.json"), load::PersistenceError);
    }
    BOOST_CHECK_EXCEPTION((void)load::load_state(scratch.root / "missing.json"),
        load::PersistenceError, [](const auto& error) {
            return std::string(error.what()).find("missing.json") != std::string::npos;
        });
}

BOOST_AUTO_TEST_CASE(failed_serialization_and_failed_rename_preserve_previous_files) {
    Scratch scratch;
    auto state = example_state();
    load::save_state(scratch.root / "state.json", state);
    const auto original = scratch.read("state.json");
    state.meta.session_id = std::string(1, '\xff');
    BOOST_CHECK_THROW(load::save_state(scratch.root / "state.json", state), load::PersistenceError);
    BOOST_TEST(scratch.read("state.json") == original);
    state = example_state();
    state.extras = Json{{"nan", std::numeric_limits<double>::quiet_NaN()}};
    BOOST_CHECK_THROW(load::save_state(scratch.root / "state.json", state), load::PersistenceError);
    BOOST_TEST(scratch.read("state.json") == original);
    fs::create_directory(scratch.root / "occupied");
    scratch.write("occupied/keep", "unchanged");
    BOOST_CHECK_THROW(load::save_state(scratch.root / "occupied", example_state()), load::PersistenceError);
    BOOST_TEST(scratch.read("occupied/keep") == "unchanged");
    scratch.no_temporary_files();
}

BOOST_AUTO_TEST_CASE(readable_preserves_chronology_and_is_not_a_snapshot) {
    Scratch scratch;
    const auto state = example_state();
    const Json before = state;
    load::save_state(scratch.root / "state.md", state, load::StateFormat::Readable);
    const auto markdown = scratch.read("state.md");
    for (const auto* text : {"## Session", "session-42", "## Loop progress", "projection",
                            "Pending tool result 1", "## System prompt", "Be precise.",
                            "## Tools", "## Conversation", "Commit sequence: 7",
                            "Reasoning", "call-1", "tool output", "cache_hit", "external_status"}) {
        BOOST_TEST(markdown.find(text) != std::string::npos);
    }
    BOOST_TEST(markdown.find("#### User input") < markdown.find("##### Model response"));
    BOOST_TEST(markdown.find("##### Model response") < markdown.find("##### Tool result 1"));
    BOOST_CHECK(Json(state) == before);
    BOOST_CHECK_THROW((void)load::load_state(scratch.root / "state.md"), load::PersistenceError);
}

BOOST_AUTO_TEST_CASE(readable_clips_json_but_keeps_later_sections_and_plain_text) {
    Scratch scratch;
    auto state = example_state();
    const std::string huge(15000, 'z');
    auto& response = state.turns[0].agent_loop_step[0].model_response;
    response.invokes->front().arguments = {
        {"long", huge}, {"array", {1, 2, 3, 4, 5, 6}},
        {"deep", {{"nested", {{"hidden", true}}}}}
    };
    response.content[0].raw = Json({{"encoded", huge}}).dump();
    state.turns[0].user_input.content[0].raw = "plain-prose-" + std::string(9000, 'p');
    state.turns.push_back({});
    state.turns.back().user_input.content.push_back({model_io::ContentType::Text, "LAST TURN", {}});
    load::ReadableOptions options;
    options.max_json_string_bytes = 24;
    options.max_json_items = 3;
    options.max_json_depth = 2;
    options.max_json_block_bytes = 512;
    const Json before = state;
    load::save_state(scratch.root / "state.md", state, load::StateFormat::Readable, options);
    const auto markdown = scratch.read("state.md");
    BOOST_TEST(markdown.find(huge) == std::string::npos);
    BOOST_TEST(markdown.find("JSON preview truncated") != std::string::npos);
    BOOST_TEST(markdown.find("depth limit") != std::string::npos);
    BOOST_TEST(markdown.find("entries omitted") != std::string::npos);
    BOOST_TEST(markdown.find(state.turns[0].user_input.content[0].raw) != std::string::npos);
    BOOST_TEST(markdown.find("LAST TURN") != std::string::npos);
    BOOST_TEST(markdown.find("## Session extras") != std::string::npos);
    BOOST_CHECK(Json(state) == before);
}

BOOST_AUTO_TEST_CASE(readable_bounds_blocks_and_preserves_utf8_and_fences) {
    Scratch scratch;
    auto state = example_state();
    state.extras = Json{{"utf8", "中文文字测试"}, {"values", Json::array()}};
    for (int index = 0; index < 100; ++index) {
        (*state.extras)["values"].push_back({{"long-value", std::string(500, 'x')}});
    }
    state.turns[0].user_input.content[0].raw = "```\n# embedded heading\n````\nend";
    state.turns[0].user_input.content.push_back({model_io::ContentType::Binary, std::string(12000, 'b'), {}});
    load::ReadableOptions options;
    options.max_json_string_bytes = 7;
    options.max_json_block_bytes = 128;
    load::save_state(scratch.root / "state.md", state, load::StateFormat::Readable, options);
    const auto markdown = scratch.read("state.md");
    BOOST_CHECK_NO_THROW(Json(markdown).dump()); // Strict UTF-8 validation.
    BOOST_TEST(markdown.find("中文") != std::string::npos);
    BOOST_TEST(markdown.find("`````text") != std::string::npos);
    BOOST_TEST(markdown.find("12000 base64 bytes") != std::string::npos);
    BOOST_TEST(markdown.find(std::string(1000, 'b')) == std::string::npos);
    std::size_t start = 0;
    while ((start = markdown.find("```json\n", start)) != std::string::npos) {
        start += 8;
        const auto end = markdown.find("\n```", start);
        BOOST_REQUIRE(end != std::string::npos);
        BOOST_TEST(end - start <= options.max_json_block_bytes);
        start = end + 4;
    }
}

BOOST_AUTO_TEST_CASE(invalid_options_and_readable_failure_do_not_replace_existing_file) {
    Scratch scratch;
    scratch.write("state.md", "old export");
    auto state = example_state();
    load::ReadableOptions options;
    options.max_json_depth = 0;
    BOOST_CHECK_THROW(load::save_state(scratch.root / "state.md", state,
        load::StateFormat::Readable, options), load::PersistenceError);
    options = {};
    state.extras = Json{{"invalid", std::string(1, '\xff')}};
    BOOST_CHECK_THROW(load::save_state(scratch.root / "state.md", state,
        load::StateFormat::Readable, options), load::PersistenceError);
    BOOST_TEST(scratch.read("state.md") == "old export");
    scratch.no_temporary_files();
}
