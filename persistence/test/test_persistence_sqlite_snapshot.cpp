#define BOOST_TEST_MODULE PersistenceSQLiteSnapshot
#include <boost/test/unit_test.hpp>

#include "sqlite/database.hpp"
#include "sqlite_schema.hpp"
#include "sqlite_snapshot.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <sqlite3.h>

namespace fs = std::filesystem;
using Json = nlohmann::json;
using model_io::AgentInputState;
using model_io::AgentLoopStep;
using model_io::Content;
using model_io::ContentType;
using model_io::InvokeQuery;
using model_io::InvokeReturn;
using model_io::InvokeSecurity;
using model_io::InvokeType;
using model_io::LoopPhase;
using model_io::LoopProgress;
using model_io::LoopStatus;
using model_io::MessageItem;
using model_io::MessageItemType;
using model_io::Modality;
using model_io::RetainPriority;
using model_io::SectionStability;
using model_io::SessionStatus;
using model_io::TokenCost;
using model_io::UserLoopStep;
using persistence::sqlite_snapshot::Reason;
using persistence::sqlite_snapshot::SnapshotError;
using persistence::sqlite_snapshot::SnapshotKind;

namespace {

struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_sqlite_snapshot_" + std::to_string(::getpid()));

    Scratch() {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    std::string database() const {
        return (root / "state.sqlite3").string();
    }
};

int code(Reason reason) {
    return static_cast<int>(reason);
}

void open(sqlite::Database& database, std::string_view session = "session-1") {
    persistence::sqlite_schema::open_store(
        database, session, "2026-10-18T00:00:00Z");
}

std::int64_t scalar(
    sqlite::Database& database, std::string_view sql,
    std::int64_t parameter = 0, bool bind = false) {
    auto query = database.prepare(sql);
    if (bind) query.bind_int64(1, parameter);
    BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
    return query.column_int64(0);
}

std::string text(
    sqlite::Database& database, std::string_view sql,
    std::int64_t parameter = 0, bool bind = false) {
    auto query = database.prepare(sql);
    if (bind) query.bind_int64(1, parameter);
    BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
    return query.column_text(0);
}

template<typename Function>
void expect_snapshot_error(Function&& function, Reason expected) {
    try {
        function();
        BOOST_FAIL("expected SnapshotError");
    } catch (const SnapshotError& error) {
        BOOST_TEST(code(error.reason()) == code(expected));
    }
}

Content text_content(std::string raw) {
    return Content{ContentType::Text, std::move(raw), {}, Modality::Text};
}

/** A minimal valid state: no prompt sections, tools, turns, loop or extras. */
AgentInputState minimal_state(std::string session = "session-1") {
    AgentInputState state;
    state.meta.session_id = std::move(session);
    state.meta.created_at = "2026-10-18T01:02:03Z";
    state.meta.updated_at = "2026-10-18T04:05:06Z";
    return state;
}

InvokeQuery query(
    std::string id, std::string name, Json arguments,
    InvokeType type = InvokeType::ReadOnly,
    InvokeSecurity security = InvokeSecurity::Trusted) {
    InvokeQuery value;
    value.id = std::move(id);
    value.name = std::move(name);
    value.arguments = std::move(arguments);
    value.type = type;
    value.security = security;
    return value;
}

/** A representative state exercising every mapped field and encoding rule. */
AgentInputState rich_state(std::string session = "session-1") {
    AgentInputState state = minimal_state(std::move(session));
    state.meta.schema_version = 7;
    state.meta.status = SessionStatus::Failed;
    state.meta.error =
        Json{{"kind", "fatal"}, {"message", "boom"}, {"at", "2026-10-18T04:00:00Z"}};

    state.system_prompt.heading_level = 3;
    state.system_prompt.add_section(
        "identity", "Identity", "You are precise.",
        SectionStability::Immutable);
    state.system_prompt.add_section(
        "tools", "Tools", "Available tools:", SectionStability::Growing);
    state.system_prompt.append("tools", "- probe: inspect a value");
    state.system_prompt.add_section(
        "summary", "Summary", "A volatile tail.", SectionStability::Volatile);

    model_io::Invocable tool;
    tool.name = "probe";
    tool.description = "Inspect a local value.";
    tool.argument_schema = Json::parse(
        R"({"type":"object","properties":{"value":{"type":"string"}}})");
    tool.remote_type = "mcp";
    tool.extras = Json{{"provider", "test"}, {"nullable", nullptr}};
    state.tools.push_back(tool);
    model_io::Invocable bare;
    bare.name = "bare";
    state.tools.push_back(bare);

    UserLoopStep turn;
    turn.retain_priority = RetainPriority::Pinned;
    turn.extras = Json{{"turn_note", nullptr}};
    turn.user_input.type = MessageItemType::UserInput;
    turn.user_input.role = "user";
    turn.user_input.content.push_back(
        text_content(std::string("hello\0world", 11)));
    Content binary;
    binary.type = ContentType::Binary;
    binary.modality = Modality::Image;
    binary.raw = "iVBORw0KGgo=";
    binary.extras = Json{{"detail", "high"}};
    turn.user_input.content.push_back(binary);
    Content reference;
    reference.type = ContentType::ExternalRef;
    reference.modality = Modality::Document;
    reference.raw = "https://example.invalid/doc.pdf";
    reference.extras = Json(nullptr);
    turn.user_input.content.push_back(reference);

    AgentLoopStep step;
    step.retain_priority = RetainPriority::Discardable;
    step.commit_sequence = 42;
    step.extras = Json::array({"step", 1});
    step.model_response.type = MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.content.push_back(text_content("answer"));
    step.model_response.reasoning = text_content("reasoning text");
    step.model_response.action_status = text_content("in_progress");

    InvokeQuery proposed = query(
        "call-a", "probe", Json{{"value", "x"}});
    proposed.extras = Json{{"origin", "model"}};
    InvokeQuery duplicate = proposed;
    duplicate.arguments = Json{{"value", "y"}};
    InvokeQuery empty = query(
        "", "bare", Json::object(), InvokeType::SerialWrite,
        InvokeSecurity::RequireConfirm);
    step.model_response.invokes =
        std::vector<InvokeQuery>{proposed, duplicate, empty};
    step.model_response.cost =
        TokenCost{std::numeric_limits<std::uint64_t>::max(),
                  std::numeric_limits<std::uint64_t>::max() - 1, 0};
    step.model_response.extras =
        Json{{"provider", "deepseek"}, {"note", nullptr}};

    // The first returned result carries its own normalized query, which is a
    // different fact from the proposed call with the same wire id.
    MessageItem result;
    result.type = MessageItemType::InvokeReturn;
    result.role = "tool";
    result.content.push_back(text_content("result-a"));
    InvokeReturn returned;
    returned.query = proposed;
    returned.query.arguments = Json{{"value", "normalized"}};
    returned.query.extras = Json(nullptr);
    returned.output = text_content("result-a");
    returned.extras = Json{{"duration_ms", 12}};
    result.invoke_return = returned;
    result.extras = Json(nullptr);

    MessageItem binary_result;
    binary_result.type = MessageItemType::InvokeReturn;
    binary_result.role = "tool";
    binary_result.content.push_back(
        Content{ContentType::Binary, "AAECAw==",
                Json{{"encoding", "base64"}}, Modality::Audio});
    InvokeReturn returned_binary;
    returned_binary.query = empty;
    returned_binary.output = binary_result.content.front();
    binary_result.invoke_return = returned_binary;

    step.invoke_returns =
        std::vector<MessageItem>{std::move(result), std::move(binary_result)};
    turn.agent_loop_step.push_back(std::move(step));

    AgentLoopStep final_step;
    final_step.model_response.type = MessageItemType::ModelResponse;
    final_step.model_response.role = "assistant";
    final_step.model_response.content.push_back(text_content("final"));
    turn.agent_loop_step.push_back(std::move(final_step));
    state.turns.push_back(std::move(turn));

    UserLoopStep empty_turn;
    empty_turn.user_input.type = MessageItemType::UserInput;
    empty_turn.user_input.role = "user";
    state.turns.push_back(std::move(empty_turn));

    LoopProgress loop;
    loop.status = LoopStatus::AutoCompactRequired;
    loop.phase = LoopPhase::Projection;
    loop.completed_exchanges = 5;
    loop.committed_response_sequence =
        std::numeric_limits<std::uint64_t>::max();
    loop.error = "";
    InvokeReturn pending;
    pending.query = empty;
    pending.query.arguments = Json{{"value", "pending"}};
    pending.query.extras = Json(nullptr);
    pending.output = text_content("pending-output");
    pending.output.extras = Json(nullptr);
    pending.extras = Json(nullptr);
    loop.pending_results.push_back(pending);
    InvokeReturn pending_second;
    pending_second.query = proposed;
    pending_second.output = Content{
        ContentType::ExternalRef, "https://example.invalid", {},
        Modality::Image};
    loop.pending_results.push_back(pending_second);
    state.loop = loop;

    state.extras = Json{
        {"external_status",
         Json{{"plugin", Json{{"cursor", 7}, {"enabled", true}}}}},
        {"events",
         Json::array({Json{{"source", "plugin"},
                           {"key", "started"},
                           {"at", "2026-10-18T04:00:00Z"},
                           {"detail", Json{{"n", 1}}}}})},
        {"unknown", nullptr},
        {"scalar", 42},
        {"array", Json::array({1, "two", false, nullptr})},
    };
    return state;
}

void expect_equal(const AgentInputState& actual, const AgentInputState& expected) {
    BOOST_TEST(Json(actual) == Json(expected));
}

} // namespace

BOOST_AUTO_TEST_CASE(round_trips_a_complete_state) {
    sqlite::Database database(":memory:");
    open(database);
    const auto state = rich_state();
    BOOST_TEST(!persistence::sqlite_snapshot::load_current(database, "session-1"));

    const auto snapshot_id =
        persistence::sqlite_snapshot::replace_current(database, "session-1", state);
    const auto loaded =
        persistence::sqlite_snapshot::load_current(database, "session-1");
    BOOST_REQUIRE(loaded);
    expect_equal(*loaded, state);
    expect_equal(
        persistence::sqlite_snapshot::decode_snapshot(database, snapshot_id),
        state);

    BOOST_TEST(
        loaded->turns.at(0).user_input.content.at(0).raw
        == std::string("hello\0world", 11));
    BOOST_TEST(
        loaded->turns.at(0)
                .agent_loop_step.at(0)
                .model_response.cost->prompt
            == std::numeric_limits<std::uint64_t>::max());
    BOOST_TEST(
        loaded->loop->committed_response_sequence
        == std::numeric_limits<std::uint64_t>::max());

    // Scalars that must not pass through a JSON number use 20-digit text.
    BOOST_TEST(
        text(database, "SELECT prompt_u64 FROM message_costs LIMIT 1")
        == "18446744073709551615");
    BOOST_TEST(
        text(database, "SELECT generated_u64 FROM message_costs LIMIT 1")
        == "18446744073709551614");
    BOOST_TEST(
        text(database, "SELECT cache_hit_u64 FROM message_costs LIMIT 1")
        == "00000000000000000000");
    BOOST_TEST(
        text(database, "SELECT completed_exchanges_u64 FROM loop_progress")
        == "00000000000000000005");
    BOOST_TEST(
        text(database,
             "SELECT committed_response_sequence_u64 FROM loop_progress")
        == "18446744073709551615");

    BOOST_TEST(
        text(database, "SELECT kind FROM snapshots WHERE kind = 'current'")
        == "current");
    BOOST_TEST(scalar(database, "SELECT revision FROM snapshots") == 0);
    BOOST_TEST(text(database, "SELECT operation_id FROM snapshots") == "");
    BOOST_TEST(
        text(database, "SELECT updated_at FROM sessions") == state.meta.updated_at);
    BOOST_TEST(scalar(database, "SELECT count(*) FROM snapshots") == 1);
}

BOOST_AUTO_TEST_CASE(absent_null_and_empty_stay_distinct) {
    sqlite::Database database(":memory:");
    open(database);

    const auto absent = minimal_state();
    const auto absent_id = persistence::sqlite_snapshot::store_archive_snapshot(
        database, "session-1", absent);

    AgentInputState nulls = minimal_state();
    nulls.meta.error = Json(nullptr);
    nulls.extras = Json(nullptr);
    model_io::Invocable tool;
    tool.name = "probe";
    tool.description = "Inspect.";
    tool.remote_type = std::string(); // engaged empty string, not absent
    nulls.tools.push_back(tool);
    UserLoopStep turn;
    turn.user_input.type = MessageItemType::UserInput;
    turn.user_input.role = "user";
    AgentLoopStep step;
    step.model_response.type = MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.invokes = std::vector<InvokeQuery>{};     // present empty
    step.model_response.cost = TokenCost{0, 0, 0};                // present zero
    step.invoke_returns = std::vector<MessageItem>{};             // present empty
    turn.agent_loop_step.push_back(step);
    nulls.turns.push_back(turn);
    nulls.loop = LoopProgress{};                                  // present empty
    const auto nulls_id = persistence::sqlite_snapshot::store_archive_snapshot(
        database, "session-1", nulls);

    AgentInputState empty_array = minimal_state();
    empty_array.extras = Json::array();
    const auto array_id = persistence::sqlite_snapshot::store_archive_snapshot(
        database, "session-1", empty_array);

    // Absent: SQL NULL and no child rows.
    BOOST_TEST(
        scalar(database,
               "SELECT extras_json IS NULL FROM snapshots WHERE snapshot_id = ?1",
               absent_id, true)
        == 1);
    BOOST_TEST(
        scalar(database,
               "SELECT meta_error_json IS NULL FROM snapshots WHERE snapshot_id = ?1",
               absent_id, true)
        == 1);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM loop_progress WHERE snapshot_id = ?1",
               absent_id, true)
        == 0);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM prompt_sections WHERE snapshot_id = ?1",
               absent_id, true)
        == 0);

    // Engaged null: the JSON text `null`, never SQL NULL.
    BOOST_TEST(
        scalar(database,
               "SELECT extras_json IS NULL FROM snapshots WHERE snapshot_id = ?1",
               nulls_id, true)
        == 0);
    BOOST_TEST(
        text(database,
             "SELECT extras_json FROM snapshots WHERE snapshot_id = ?1",
             nulls_id, true)
        == "null");
    BOOST_TEST(
        text(database,
             "SELECT meta_error_json FROM snapshots WHERE snapshot_id = ?1",
             nulls_id, true)
        == "null");
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM loop_progress WHERE snapshot_id = ?1",
               nulls_id, true)
        == 1);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM pending_results WHERE snapshot_id = ?1",
               nulls_id, true)
        == 0);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM message_invokes WHERE message_id IN "
               "(SELECT message_id FROM messages WHERE snapshot_id = ?1)",
               nulls_id, true)
        == 0);
    BOOST_TEST(
        scalar(database,
               "SELECT invokes_present FROM messages WHERE snapshot_id = ?1 "
               "AND slot = 'model_response'",
               nulls_id, true)
        == 1);
    BOOST_TEST(
        scalar(database,
               "SELECT invoke_returns_present FROM agent_steps WHERE snapshot_id = ?1",
               nulls_id, true)
        == 1);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM messages WHERE snapshot_id = ?1 "
               "AND slot = 'invoke_return'",
               nulls_id, true)
        == 0);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM message_costs WHERE message_id IN "
               "(SELECT message_id FROM messages WHERE snapshot_id = ?1)",
               nulls_id, true)
        == 1);
    BOOST_TEST(
        text(database,
             "SELECT prompt_u64 FROM message_costs WHERE message_id IN "
             "(SELECT message_id FROM messages WHERE snapshot_id = ?1)",
             nulls_id, true)
        == "00000000000000000000");
    BOOST_TEST(
        scalar(database,
               "SELECT remote_type IS NULL FROM tool_definitions "
               "WHERE snapshot_id = ?1",
               nulls_id, true)
        == 0);
    BOOST_TEST(
        text(database,
             "SELECT remote_type FROM tool_definitions WHERE snapshot_id = ?1",
             nulls_id, true)
        == "");

    // Empty array is neither NULL nor an empty object.
    BOOST_TEST(
        text(database,
             "SELECT extras_json FROM snapshots WHERE snapshot_id = ?1",
             array_id, true)
        == "[]");

    const auto decoded_absent =
        persistence::sqlite_snapshot::decode_snapshot(database, absent_id);
    expect_equal(decoded_absent, absent);
    BOOST_TEST(!decoded_absent.extras.has_value());
    BOOST_TEST(!decoded_absent.meta.error.has_value());
    BOOST_TEST(!decoded_absent.loop.has_value());

    const auto decoded_nulls =
        persistence::sqlite_snapshot::decode_snapshot(database, nulls_id);
    expect_equal(decoded_nulls, nulls);
    BOOST_TEST(decoded_nulls.extras.has_value());
    BOOST_TEST(decoded_nulls.extras->is_null());
    BOOST_TEST(decoded_nulls.meta.error.has_value());
    BOOST_TEST(decoded_nulls.meta.error->is_null());
    BOOST_TEST(decoded_nulls.loop.has_value());
    BOOST_TEST(decoded_nulls.loop->pending_results.empty());
    BOOST_TEST(decoded_nulls.tools.at(0).remote_type.has_value());
    BOOST_TEST(decoded_nulls.tools.at(0).remote_type->empty());
    BOOST_TEST(
        decoded_nulls.turns.at(0)
            .agent_loop_step.at(0)
            .model_response.invokes.has_value());
    BOOST_TEST(
        decoded_nulls.turns.at(0)
            .agent_loop_step.at(0)
            .model_response.invokes->empty());
    BOOST_TEST(
        decoded_nulls.turns.at(0)
            .agent_loop_step.at(0)
            .model_response.cost.has_value());
    BOOST_TEST(
        decoded_nulls.turns.at(0)
            .agent_loop_step.at(0)
            .invoke_returns.has_value());
    BOOST_TEST(
        decoded_nulls.turns.at(0).agent_loop_step.at(0).invoke_returns->empty());

    expect_equal(
        persistence::sqlite_snapshot::decode_snapshot(database, array_id),
        empty_array);
}

BOOST_AUTO_TEST_CASE(repeated_and_empty_call_ids_stay_independent) {
    sqlite::Database database(":memory:");
    open(database);

    AgentInputState state = minimal_state();
    UserLoopStep turn;
    turn.user_input.type = MessageItemType::UserInput;
    turn.user_input.role = "user";
    AgentLoopStep step;
    step.model_response.type = MessageItemType::ModelResponse;
    step.model_response.role = "assistant";
    step.model_response.content.push_back(text_content("identical"));
    step.model_response.content.push_back(text_content("identical"));
    step.model_response.content.push_back(text_content("different"));
    step.model_response.invokes = std::vector<InvokeQuery>{
        query("same", "probe", Json{{"value", 1}}),
        query("same", "probe", Json{{"value", 2}}),
        query("", "probe", Json{{"value", 3}}),
    };
    MessageItem first;
    first.type = MessageItemType::InvokeReturn;
    first.role = "tool";
    first.content.push_back(text_content("first"));
    InvokeReturn returned_first;
    returned_first.query = query("same", "probe", Json{{"value", 10}});
    returned_first.output = text_content("first");
    first.invoke_return = returned_first;
    MessageItem second;
    second.type = MessageItemType::InvokeReturn;
    second.role = "tool";
    second.content.push_back(text_content("second"));
    InvokeReturn returned_second;
    returned_second.query = query("", "probe", Json{{"value", 20}});
    returned_second.output = text_content("second");
    second.invoke_return = returned_second;
    step.invoke_returns = std::vector<MessageItem>{first, second};
    turn.agent_loop_step.push_back(step);
    state.turns.push_back(turn);
    LoopProgress loop;
    loop.pending_results.push_back(returned_first);
    loop.pending_results.push_back(returned_first);
    loop.pending_results.at(1).query.arguments = Json{{"value", 11}};
    state.loop = loop;

    const auto snapshot_id =
        persistence::sqlite_snapshot::store_archive_snapshot(
            database, "session-1", state);

    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM message_invokes WHERE message_id IN "
               "(SELECT message_id FROM messages WHERE snapshot_id = ?1)",
               snapshot_id, true)
        == 3);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM message_invoke_returns WHERE message_id IN "
               "(SELECT message_id FROM messages WHERE snapshot_id = ?1)",
               snapshot_id, true)
        == 2);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM pending_results WHERE snapshot_id = ?1",
               snapshot_id, true)
        == 2);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM message_contents WHERE raw = 'identical' "
               "AND message_id IN (SELECT message_id FROM messages "
               "WHERE snapshot_id = ?1)",
               snapshot_id, true)
        == 2);

    // The duplicate and empty wire ids are preserved in all three places.
    std::vector<std::string> invoke_ids;
    {
        auto rows = database.prepare(
            "SELECT wire_call_id FROM message_invokes WHERE message_id IN "
            "(SELECT message_id FROM messages WHERE snapshot_id = ?1) "
            "ORDER BY ordinal");
        rows.bind_int64(1, snapshot_id);
        while (rows.step() == sqlite::StepResult::Row) {
            invoke_ids.push_back(rows.column_text(0));
        }
    }
    BOOST_REQUIRE(invoke_ids.size() == 3);
    BOOST_TEST(invoke_ids.at(0) == "same");
    BOOST_TEST(invoke_ids.at(1) == "same");
    BOOST_TEST(invoke_ids.at(2).empty());

    std::vector<std::string> pending_arguments;
    {
        auto rows = database.prepare(
            "SELECT query_arguments_json FROM pending_results "
            "WHERE snapshot_id = ?1 ORDER BY ordinal");
        rows.bind_int64(1, snapshot_id);
        while (rows.step() == sqlite::StepResult::Row) {
            pending_arguments.push_back(rows.column_text(0));
        }
    }
    BOOST_REQUIRE(pending_arguments.size() == 2);
    BOOST_TEST(pending_arguments.at(0) != pending_arguments.at(1));

    expect_equal(
        persistence::sqlite_snapshot::decode_snapshot(database, snapshot_id),
        state);
}

BOOST_AUTO_TEST_CASE(replace_current_rewrites_the_complete_graph) {
    sqlite::Database database(":memory:");
    open(database);
    const auto original = rich_state();
    const auto current_id =
        persistence::sqlite_snapshot::replace_current(database, "session-1", original);
    const auto archive_id =
        persistence::sqlite_snapshot::store_archive_snapshot(
            database, "session-1", original);

    AgentInputState replacement = original;
    replacement.meta.updated_at = "2026-10-18T09:09:09Z";
    replacement.meta.status = SessionStatus::Completed;
    replacement.meta.error.reset();
    replacement.system_prompt.rewrite("summary", "Rewritten summary.");
    replacement.tools.erase(replacement.tools.begin());
    replacement.turns.pop_back(); // remove the empty turn
    replacement.turns.at(0).user_input.content.at(0).raw = "replaced";
    replacement.loop.reset();
    replacement.extras = Json{{"only", "new"}};

    const auto replaced_id =
        persistence::sqlite_snapshot::replace_current(
            database, "session-1", replacement);
    BOOST_TEST(replaced_id == current_id);

    const auto loaded =
        persistence::sqlite_snapshot::load_current(database, "session-1");
    BOOST_REQUIRE(loaded);
    expect_equal(*loaded, replacement);

    // The archive was written before the replacement and is unchanged.
    expect_equal(
        persistence::sqlite_snapshot::decode_snapshot(database, archive_id),
        original);

    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM prompt_sections WHERE snapshot_id = ?1",
               current_id, true)
        == 3);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM tool_definitions WHERE snapshot_id = ?1",
               current_id, true)
        == 1);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM turns WHERE snapshot_id = ?1",
               current_id, true)
        == 1);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM agent_steps WHERE snapshot_id = ?1",
               current_id, true)
        == 2);
    BOOST_TEST(
        scalar(database,
               "SELECT count(*) FROM messages WHERE snapshot_id = ?1",
               current_id, true)
        == 5);
    {
        auto stale = database.prepare(
            "SELECT count(*) FROM message_contents WHERE raw = ?1 "
            "AND message_id IN (SELECT message_id FROM messages "
            "WHERE snapshot_id = ?2)");
        stale.bind_text(1, std::string("hello\0world", 11));
        stale.bind_int64(2, current_id);
        BOOST_REQUIRE(stale.step() == sqlite::StepResult::Row);
        BOOST_TEST(stale.column_int64(0) == 0);
    }
    BOOST_TEST(
        text(database, "SELECT updated_at FROM sessions")
        == replacement.meta.updated_at);
    BOOST_TEST(scalar(database, "SELECT count(*) FROM snapshots") == 2);

    // The first replacement creates the current snapshot when none exists.
    sqlite::Database fresh(":memory:");
    open(fresh);
    const auto first_id = persistence::sqlite_snapshot::replace_current(
        fresh, "session-1", minimal_state());
    const auto again = persistence::sqlite_snapshot::replace_current(
        fresh, "session-1", original);
    BOOST_TEST(again == first_id);
    const auto fresh_loaded =
        persistence::sqlite_snapshot::load_current(fresh, "session-1");
    BOOST_REQUIRE(fresh_loaded);
    expect_equal(*fresh_loaded, original);
}

BOOST_AUTO_TEST_CASE(damaged_graphs_and_cross_snapshot_rows_are_rejected) {
    sqlite::Database database(":memory:");
    open(database);
    const auto state = rich_state();
    const auto current_id =
        persistence::sqlite_snapshot::replace_current(database, "session-1", state);
    const auto archive_id =
        persistence::sqlite_snapshot::store_archive_snapshot(
            database, "session-1", state);

    // An agent step may not reference a turn owned by another snapshot.
    std::int64_t archive_turn = 0;
    {
        auto turn = database.prepare(
            "SELECT turn_id FROM turns WHERE snapshot_id = ?1 "
            "ORDER BY ordinal LIMIT 1");
        turn.bind_int64(1, archive_id);
        BOOST_REQUIRE(turn.step() == sqlite::StepResult::Row);
        archive_turn = turn.column_int64(0);
    }
    {
        auto insert = database.prepare(
            "INSERT INTO agent_steps("
            "snapshot_id, turn_id, ordinal, retain_priority, "
            "commit_sequence_u64, invoke_returns_present) "
            "VALUES(?1, ?2, 99, 'normal', '00000000000000000000', 0)");
        insert.bind_int64(1, current_id);
        insert.bind_int64(2, archive_turn);
        try {
            insert.step();
            BOOST_FAIL("cross-snapshot step must be rejected");
        } catch (const sqlite::Error& error) {
            BOOST_TEST(error.code() == SQLITE_CONSTRAINT);
            BOOST_TEST(error.extended_code() == SQLITE_CONSTRAINT_FOREIGNKEY);
        }
    }

    // Missing message cardinality is corruption, not an empty session.
    {
        const auto damaged = persistence::sqlite_snapshot::store_archive_snapshot(
            database, "session-1", state);
        std::int64_t message_id = 0;
        {
            auto query = database.prepare(
                "SELECT message_id FROM messages WHERE snapshot_id = ?1 "
                "AND slot = 'model_response' ORDER BY message_id LIMIT 1");
            query.bind_int64(1, damaged);
            BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
            message_id = query.column_int64(0);
        }
        auto remove = database.prepare("DELETE FROM messages WHERE message_id = ?1");
        remove.bind_int64(1, message_id);
        BOOST_REQUIRE(remove.step() == sqlite::StepResult::Done);
        expect_snapshot_error(
            [&] {
                persistence::sqlite_snapshot::decode_snapshot(database, damaged);
            },
            Reason::CorruptSnapshot);
    }
    {
        const auto damaged = persistence::sqlite_snapshot::store_archive_snapshot(
            database, "session-1", state);
        std::int64_t message_id = 0;
        {
            auto query = database.prepare(
                "SELECT message_id FROM messages WHERE snapshot_id = ?1 "
                "AND slot = 'user_input' ORDER BY message_id LIMIT 1");
            query.bind_int64(1, damaged);
            BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
            message_id = query.column_int64(0);
        }
        auto remove = database.prepare("DELETE FROM messages WHERE message_id = ?1");
        remove.bind_int64(1, message_id);
        BOOST_REQUIRE(remove.step() == sqlite::StepResult::Done);
        expect_snapshot_error(
            [&] {
                persistence::sqlite_snapshot::decode_snapshot(database, damaged);
            },
            Reason::CorruptSnapshot);
    }

    // Invalid JSON in an opaque column is reported, never silently dropped.
    {
        const auto damaged = persistence::sqlite_snapshot::store_archive_snapshot(
            database, "session-1", state);
        auto update = database.prepare(
            "UPDATE snapshots SET extras_json = '{not json' "
            "WHERE snapshot_id = ?1");
        update.bind_int64(1, damaged);
        BOOST_REQUIRE(update.step() == sqlite::StepResult::Done);
        expect_snapshot_error(
            [&] {
                persistence::sqlite_snapshot::decode_snapshot(database, damaged);
            },
            Reason::CorruptSnapshot);
    }

    // invoke rows contradicting invokes_present are corruption.
    {
        AgentInputState without_invokes = minimal_state();
        UserLoopStep turn;
        turn.user_input.type = MessageItemType::UserInput;
        turn.user_input.role = "user";
        AgentLoopStep step;
        step.model_response.type = MessageItemType::ModelResponse;
        step.model_response.role = "assistant";
        turn.agent_loop_step.push_back(step);
        without_invokes.turns.push_back(turn);
        const auto damaged = persistence::sqlite_snapshot::store_archive_snapshot(
            database, "session-1", without_invokes);
        std::int64_t message_id = 0;
        {
            auto query = database.prepare(
                "SELECT message_id FROM messages WHERE snapshot_id = ?1 "
                "AND slot = 'model_response'");
            query.bind_int64(1, damaged);
            BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
            message_id = query.column_int64(0);
        }
        auto insert = database.prepare(
            "INSERT INTO message_invokes("
            "message_id, ordinal, query_type, query_security, wire_call_id, "
            "name, arguments_json, extras_json) "
            "VALUES(?1, 0, 'read_only', 'default_deny', 'x', 'probe', '{}', NULL)");
        insert.bind_int64(1, message_id);
        BOOST_REQUIRE(insert.step() == sqlite::StepResult::Done);
        expect_snapshot_error(
            [&] {
                persistence::sqlite_snapshot::decode_snapshot(database, damaged);
            },
            Reason::CorruptSnapshot);
    }

    expect_snapshot_error(
        [&] { persistence::sqlite_snapshot::decode_snapshot(database, 999999); },
        Reason::SnapshotNotFound);

    auto mismatched = minimal_state("session-2");
    expect_snapshot_error(
        [&] {
            persistence::sqlite_snapshot::replace_current(
                database, "session-1", mismatched);
        },
        Reason::SessionMismatch);
    expect_snapshot_error(
        [&] {
            persistence::sqlite_snapshot::store_archive_snapshot(
                database, "session-1", mismatched);
        },
        Reason::SessionMismatch);

    auto unknown = minimal_state("session-x");
    expect_snapshot_error(
        [&] {
            persistence::sqlite_snapshot::replace_current(
                database, "session-x", unknown);
        },
        Reason::UnknownSession);
    expect_snapshot_error(
        [&] {
            persistence::sqlite_snapshot::load_current(database, "session-x");
        },
        Reason::UnknownSession);

    // A second current snapshot is refused by the partial unique index.
    BOOST_CHECK_THROW(
        persistence::sqlite_snapshot::encode_snapshot(
            database, "session-1", state, SnapshotKind::Current),
        sqlite::Error);
}

BOOST_AUTO_TEST_CASE(read_only_connection_loads_the_current_snapshot) {
    Scratch scratch;
    const auto path = scratch.database();
    const auto state = rich_state();
    {
        sqlite::Database writer(path);
        open(writer);
        persistence::sqlite_snapshot::replace_current(writer, "session-1", state);
    }
    {
        sqlite::Database reader(path, sqlite::OpenMode::ReadOnly);
        const auto identity = persistence::sqlite_schema::inspect_store(
            reader, "session-1");
        BOOST_TEST(identity.session_id == "session-1");
        const auto loaded =
            persistence::sqlite_snapshot::load_current(reader, "session-1");
        BOOST_REQUIRE(loaded);
        expect_equal(*loaded, state);
        BOOST_CHECK_THROW(
            persistence::sqlite_snapshot::replace_current(
                reader, "session-1", state),
            sqlite::Error);
    }
}
