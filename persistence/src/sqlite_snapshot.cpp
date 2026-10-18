#include "sqlite_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace persistence::sqlite_snapshot {
namespace {

using Json = nlohmann::json;

[[noreturn]] void corrupt(const std::string& message) {
    throw SnapshotError(Reason::CorruptSnapshot, message);
}

[[noreturn]] void invalid_state(const std::string& message) {
    throw SnapshotError(Reason::InvalidState, message);
}

/** Encode an enum through the dataclass JSON name, never through an integer. */
template<typename Enum>
std::string enum_name(Enum value, const char* column) {
    try {
        return Json(value).get<std::string>();
    } catch (const std::exception& error) {
        invalid_state(std::string("cannot encode ") + column + ": " + error.what());
    }
}

/** Decode an enum through the dataclass name; unknown names are corruption. */
template<typename Enum>
Enum enum_value(std::string_view name, const char* column) {
    try {
        return Json(std::string(name)).get<Enum>();
    } catch (const std::exception&) {
        corrupt(
            std::string("unknown ") + column + " value '" + std::string(name)
            + "'");
    }
}

[[noreturn]] void invalid_json(const char* column) {
    corrupt(std::string("invalid JSON in ") + column);
}

std::string dump_json(const Json& value, const char* column) {
    try {
        return value.dump();
    } catch (const Json::exception& error) {
        invalid_state(
            std::string("cannot encode ") + column + ": " + error.what());
    }
}

Json parse_json(const std::string& text, const char* column) {
    try {
        return Json::parse(text);
    } catch (const Json::exception&) {
        invalid_json(column);
    }
}

/** NULL stays absent; the JSON text `null` is an engaged null value. */
std::optional<std::string> encode_optional_json(
    const std::optional<Json>& value) {
    if (!value) return std::nullopt;
    if (value->is_null()) return std::string("null");
    return dump_json(*value, "JSON value");
}

std::optional<Json> decode_optional_json(
    const std::optional<std::string>& text, const char* column) {
    if (!text) return std::nullopt;
    return parse_json(*text, column);
}

std::string format_u64(std::uint64_t value) {
    char buffer[21]{};
    std::snprintf(buffer, sizeof(buffer), "%020llu",
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::uint64_t parse_u64(std::string_view text, const char* column) {
    if (text.size() != 20) {
        corrupt(std::string(column) + " is not a 20-digit uint64");
    }
    std::uint64_t value = 0;
    for (const char byte : text) {
        if (byte < '0' || byte > '9') {
            corrupt(std::string(column) + " is not a decimal uint64");
        }
        const auto digit = static_cast<std::uint64_t>(byte - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            corrupt(std::string(column) + " overflows uint64");
        }
        value = value * 10 + digit;
    }
    return value;
}

std::optional<std::string> optional_column(
    const sqlite::Statement& statement, int column) {
    if (statement.column_is_null(column)) return std::nullopt;
    return statement.column_text(column);
}

void bind_optional_text(
    sqlite::Statement& statement, int index,
    const std::optional<std::string>& text) {
    if (text) {
        statement.bind_text(index, *text);
    } else {
        statement.bind_null(index);
    }
}

void bind_optional_int64(
    sqlite::Statement& statement, int index,
    const std::optional<std::int64_t>& value) {
    if (value) {
        statement.bind_int64(index, *value);
    } else {
        statement.bind_null(index);
    }
}

void step_done(sqlite::Statement& statement, const char* operation) {
    if (statement.step() != sqlite::StepResult::Done) {
        throw std::runtime_error(operation);
    }
}

std::int64_t insert_rowid(
    sqlite::Database& database, sqlite::Statement& statement,
    const char* operation) {
    step_done(statement, operation);
    return database.last_insert_rowid();
}

/** Confirm the connection enforces the schema's foreign keys. */
void require_foreign_keys(sqlite::Database& database) {
    database.execute("PRAGMA foreign_keys = ON");
    auto query = database.prepare("PRAGMA foreign_keys");
    if (query.step() != sqlite::StepResult::Row || query.column_int64(0) != 1) {
        throw std::logic_error(
            "SQLite foreign key enforcement could not be enabled");
    }
}

void require_session(sqlite::Database& database, std::string_view session_id) {
    auto query = database.prepare(
        "SELECT count(*) FROM sessions WHERE session_id = ?1");
    query.bind_text(1, session_id);
    if (query.step() != sqlite::StepResult::Row || query.column_int64(0) != 1) {
        throw SnapshotError(
            Reason::UnknownSession,
            "session store has no row for session '" + std::string(session_id)
                + "'");
    }
}

void require_matching_session(
    std::string_view session_id, const model_io::MetaInfo& meta) {
    if (meta.session_id != session_id) {
        throw SnapshotError(
            Reason::SessionMismatch,
            "state belongs to session '" + meta.session_id + "', not '"
                + std::string(session_id) + "'");
    }
}

std::string kind_name(SnapshotKind kind) {
    switch (kind) {
    case SnapshotKind::Current:
        return "current";
    case SnapshotKind::Archive:
        return "archive";
    }
    throw std::logic_error("invalid SnapshotKind value");
}

/** Write the snapshot header; an explicit id keeps a replacement's identity. */
std::int64_t insert_snapshot_header(
    sqlite::Database& database, std::string_view session_id,
    const model_io::AgentInputState& state, SnapshotKind kind,
    std::optional<std::int64_t> explicit_id) {
    const auto error = encode_optional_json(state.meta.error);
    const auto extras = encode_optional_json(state.extras);
    std::string sql =
        "INSERT INTO snapshots("
        "snapshot_id, session_id, kind, revision, operation_id, "
        "meta_schema_version, session_status, meta_error_json, created_at, "
        "updated_at, heading_level, extras_json) VALUES(";
    sql += explicit_id ? "?1, ?2, ?3, 0, '', ?4, ?5, ?6, ?7, ?8, ?9, ?10)"
                       : "NULL, ?1, ?2, 0, '', ?3, ?4, ?5, ?6, ?7, ?8, ?9)";
    auto insert = database.prepare(sql);
    int index = 1;
    if (explicit_id) insert.bind_int64(index++, *explicit_id);
    insert.bind_text(index++, session_id);
    insert.bind_text(index++, kind_name(kind));
    insert.bind_int64(index++, static_cast<std::int64_t>(state.meta.schema_version));
    insert.bind_text(index++, enum_name(state.meta.status, "meta.status"));
    bind_optional_text(insert, index++, error);
    insert.bind_text(index++, state.meta.created_at);
    insert.bind_text(index++, state.meta.updated_at);
    insert.bind_int64(
        index++, static_cast<std::int64_t>(state.system_prompt.heading_level));
    bind_optional_text(insert, index, extras);
    if (explicit_id) {
        step_done(insert, "cannot insert snapshot header");
        return *explicit_id;
    }
    return insert_rowid(database, insert, "cannot insert snapshot header");
}

void insert_prompt_sections(
    sqlite::Database& database, std::int64_t snapshot_id,
    const model_io::PromptTemplate& prompt) {
    std::int64_t ordinal = 0;
    for (const auto& section : prompt) {
        auto insert = database.prepare(
            "INSERT INTO prompt_sections("
            "snapshot_id, ordinal, name, title, stability, body) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
        insert.bind_int64(1, snapshot_id);
        insert.bind_int64(2, ordinal++);
        insert.bind_text(3, section.name);
        insert.bind_text(4, section.title);
        insert.bind_text(
            5, enum_name(section.stability, "prompt_sections.stability"));
        insert.bind_text(6, section.text);
        step_done(insert, "cannot insert a prompt section");
    }
}

void insert_tools(
    sqlite::Database& database, std::int64_t snapshot_id,
    const std::vector<model_io::Invocable>& tools) {
    std::int64_t ordinal = 0;
    for (const auto& tool : tools) {
        const auto extras = encode_optional_json(tool.extras);
        auto insert = database.prepare(
            "INSERT INTO tool_definitions("
            "snapshot_id, ordinal, name, description, argument_schema_json, "
            "remote_type, extras_json) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
        insert.bind_int64(1, snapshot_id);
        insert.bind_int64(2, ordinal++);
        insert.bind_text(3, tool.name);
        insert.bind_text(4, tool.description);
        insert.bind_text(
            5, dump_json(
                   tool.argument_schema,
                   "tool_definitions.argument_schema_json"));
        bind_optional_text(insert, 6, tool.remote_type);
        bind_optional_text(insert, 7, extras);
        step_done(insert, "cannot insert a tool definition");
    }
}

void insert_content(
    sqlite::Database& database, std::int64_t message_id, const char* slot,
    std::int64_t ordinal, const model_io::Content& content) {
    const auto extras = encode_optional_json(content.extras);
    auto insert = database.prepare(
        "INSERT INTO message_contents("
        "message_id, slot, ordinal, type, modality, raw, extras_json) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    insert.bind_int64(1, message_id);
    insert.bind_text(2, slot);
    insert.bind_int64(3, ordinal);
    insert.bind_text(4, enum_name(content.type, "content.type"));
    insert.bind_text(5, enum_name(content.modality, "content.modality"));
    insert.bind_text(6, content.raw);
    bind_optional_text(insert, 7, extras);
    step_done(insert, "cannot insert message content");
}

void insert_invoke_query(
    sqlite::Database& database, const char* table, std::int64_t message_id,
    std::int64_t ordinal, const model_io::InvokeQuery& query) {
    const auto extras = encode_optional_json(query.extras);
    const std::string sql =
        std::string("INSERT INTO ") + table
        + "(message_id, ordinal, query_type, query_security, wire_call_id, "
          "name, arguments_json, extras_json) "
          "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)";
    auto insert = database.prepare(sql);
    insert.bind_int64(1, message_id);
    insert.bind_int64(2, ordinal);
    insert.bind_text(3, enum_name(query.type, "invoke.type"));
    insert.bind_text(4, enum_name(query.security, "invoke.security"));
    insert.bind_text(5, query.id);
    insert.bind_text(6, query.name);
    insert.bind_text(
        7, dump_json(query.arguments, "message_invokes.arguments_json"));
    bind_optional_text(insert, 8, extras);
    step_done(insert, "cannot insert an invoke query");
}

std::int64_t insert_message(
    sqlite::Database& database, std::int64_t snapshot_id,
    std::optional<std::int64_t> turn_id, std::optional<std::int64_t> step_id,
    const char* slot, std::int64_t ordinal,
    const model_io::MessageItem& message) {
    const auto extras = encode_optional_json(message.extras);
    auto insert = database.prepare(
        "INSERT INTO messages("
        "snapshot_id, turn_id, step_id, slot, ordinal, type, role, "
        "invokes_present, extras_json) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
    insert.bind_int64(1, snapshot_id);
    bind_optional_int64(insert, 2, turn_id);
    bind_optional_int64(insert, 3, step_id);
    insert.bind_text(4, slot);
    insert.bind_int64(5, ordinal);
    insert.bind_text(6, enum_name(message.type, "message.type"));
    insert.bind_text(7, message.role);
    insert.bind_int64(8, message.invokes ? 1 : 0);
    bind_optional_text(insert, 9, extras);
    const auto message_id =
        insert_rowid(database, insert, "cannot insert a message");

    std::int64_t content_ordinal = 0;
    for (const auto& content : message.content) {
        insert_content(database, message_id, "body", content_ordinal++, content);
    }
    if (message.reasoning) {
        insert_content(database, message_id, "reasoning", 0, *message.reasoning);
    }
    if (message.action_status) {
        insert_content(
            database, message_id, "action_status", 0, *message.action_status);
    }
    if (message.invokes) {
        std::int64_t ordinal = 0;
        for (const auto& query : *message.invokes) {
            insert_invoke_query(
                database, "message_invokes", message_id, ordinal++, query);
        }
    }
    if (message.cost) {
        auto insert_cost = database.prepare(
            "INSERT INTO message_costs("
            "message_id, prompt_u64, generated_u64, cache_hit_u64) "
            "VALUES(?1, ?2, ?3, ?4)");
        insert_cost.bind_int64(1, message_id);
        insert_cost.bind_text(2, format_u64(message.cost->prompt));
        insert_cost.bind_text(3, format_u64(message.cost->generated));
        insert_cost.bind_text(4, format_u64(message.cost->cache_hit));
        step_done(insert_cost, "cannot insert a message cost");
    }
    if (message.invoke_return) {
        // At most one record per message: message_id is the primary key.
        const auto& result = *message.invoke_return;
        const auto query_extras = encode_optional_json(result.query.extras);
        const auto output_extras = encode_optional_json(result.output.extras);
        const auto result_extras = encode_optional_json(result.extras);
        auto insert_return = database.prepare(
            "INSERT INTO message_invoke_returns("
            "message_id, query_type, query_security, wire_call_id, query_name, "
            "query_arguments_json, query_extras_json, output_type, "
            "output_modality, output_raw, output_extras_json, extras_json) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)");
        insert_return.bind_int64(1, message_id);
        insert_return.bind_text(
            2, enum_name(result.query.type, "invoke.type"));
        insert_return.bind_text(
            3, enum_name(result.query.security, "invoke.security"));
        insert_return.bind_text(4, result.query.id);
        insert_return.bind_text(5, result.query.name);
        insert_return.bind_text(
            6, dump_json(
                   result.query.arguments,
                   "message_invoke_returns.query_arguments_json"));
        bind_optional_text(insert_return, 7, query_extras);
        insert_return.bind_text(
            8, enum_name(result.output.type, "content.type"));
        insert_return.bind_text(
            9, enum_name(result.output.modality, "content.modality"));
        insert_return.bind_text(10, result.output.raw);
        bind_optional_text(insert_return, 11, output_extras);
        bind_optional_text(insert_return, 12, result_extras);
        step_done(
            insert_return, "cannot insert a message's invoke return");
    }
    return message_id;
}

void insert_loop(
    sqlite::Database& database, std::int64_t snapshot_id,
    const model_io::LoopProgress& loop) {
    auto insert = database.prepare(
        "INSERT INTO loop_progress("
        "snapshot_id, status, phase, completed_exchanges_u64, "
        "committed_response_sequence_u64, error) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
    insert.bind_int64(1, snapshot_id);
    insert.bind_text(2, enum_name(loop.status, "loop.status"));
    insert.bind_text(3, enum_name(loop.phase, "loop.phase"));
    insert.bind_text(
        4, format_u64(static_cast<std::uint64_t>(loop.completed_exchanges)));
    insert.bind_text(5, format_u64(loop.committed_response_sequence));
    insert.bind_text(6, loop.error);
    step_done(insert, "cannot insert loop progress");

    std::int64_t ordinal = 0;
    for (const auto& result : loop.pending_results) {
        const auto query_extras = encode_optional_json(result.query.extras);
        const auto output_extras = encode_optional_json(result.output.extras);
        const auto extras = encode_optional_json(result.extras);
        auto insert_result = database.prepare(
            "INSERT INTO pending_results("
            "snapshot_id, ordinal, query_type, query_security, wire_call_id, "
            "query_name, query_arguments_json, query_extras_json, output_type, "
            "output_modality, output_raw, output_extras_json, extras_json) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)");
        insert_result.bind_int64(1, snapshot_id);
        insert_result.bind_int64(2, ordinal++);
        insert_result.bind_text(
            3, enum_name(result.query.type, "invoke.type"));
        insert_result.bind_text(
            4, enum_name(result.query.security, "invoke.security"));
        insert_result.bind_text(5, result.query.id);
        insert_result.bind_text(6, result.query.name);
        insert_result.bind_text(
            7, dump_json(
                   result.query.arguments,
                   "pending_results.query_arguments_json"));
        bind_optional_text(insert_result, 8, query_extras);
        insert_result.bind_text(
            9, enum_name(result.output.type, "content.type"));
        insert_result.bind_text(
            10, enum_name(result.output.modality, "content.modality"));
        insert_result.bind_text(11, result.output.raw);
        bind_optional_text(insert_result, 12, output_extras);
        bind_optional_text(insert_result, 13, extras);
        step_done(insert_result, "cannot insert a pending result");
    }
}

void insert_turns(
    sqlite::Database& database, std::int64_t snapshot_id,
    const std::vector<model_io::UserLoopStep>& turns) {
    std::int64_t turn_ordinal = 0;
    for (const auto& turn : turns) {
        const auto extras = encode_optional_json(turn.extras);
        auto insert_turn = database.prepare(
            "INSERT INTO turns("
            "snapshot_id, ordinal, retain_priority, extras_json) "
            "VALUES(?1, ?2, ?3, ?4)");
        insert_turn.bind_int64(1, snapshot_id);
        insert_turn.bind_int64(2, turn_ordinal++);
        insert_turn.bind_text(
            3, enum_name(turn.retain_priority, "turn.retain_priority"));
        bind_optional_text(insert_turn, 4, extras);
        const auto turn_id =
            insert_rowid(database, insert_turn, "cannot insert a turn");

        insert_message(
            database, snapshot_id, turn_id, std::nullopt, "user_input", 0,
            turn.user_input);

        std::int64_t step_ordinal = 0;
        for (const auto& step : turn.agent_loop_step) {
            const auto step_extras = encode_optional_json(step.extras);
            auto insert_step = database.prepare(
                "INSERT INTO agent_steps("
                "snapshot_id, turn_id, ordinal, retain_priority, "
                "commit_sequence_u64, invoke_returns_present, extras_json) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
            insert_step.bind_int64(1, snapshot_id);
            insert_step.bind_int64(2, turn_id);
            insert_step.bind_int64(3, step_ordinal++);
            insert_step.bind_text(
                4, enum_name(step.retain_priority, "step.retain_priority"));
            insert_step.bind_text(5, format_u64(step.commit_sequence));
            insert_step.bind_int64(6, step.invoke_returns ? 1 : 0);
            bind_optional_text(insert_step, 7, step_extras);
            const auto step_id = insert_rowid(
                database, insert_step, "cannot insert an agent step");

            insert_message(
                database, snapshot_id, std::nullopt, step_id,
                "model_response", 0, step.model_response);

            if (step.invoke_returns) {
                std::int64_t result_ordinal = 0;
                for (const auto& result : *step.invoke_returns) {
                    insert_message(
                        database, snapshot_id, std::nullopt, step_id,
                        "invoke_return", result_ordinal++, result);
                }
            }
        }
    }
}

void insert_graph(
    sqlite::Database& database, std::int64_t snapshot_id,
    const model_io::AgentInputState& state) {
    insert_prompt_sections(database, snapshot_id, state.system_prompt);
    insert_tools(database, snapshot_id, state.tools);
    insert_turns(database, snapshot_id, state.turns);
    if (state.loop) {
        insert_loop(database, snapshot_id, *state.loop);
    }
}

std::optional<std::int64_t> find_current(
    sqlite::Database& database, std::string_view session_id) {
    auto query = database.prepare(
        "SELECT snapshot_id FROM snapshots "
        "WHERE session_id = ?1 AND kind = 'current'");
    query.bind_text(1, session_id);
    if (query.step() != sqlite::StepResult::Row) return std::nullopt;
    const auto snapshot_id = query.column_int64(0);
    if (query.step() == sqlite::StepResult::Row) {
        corrupt("session store has more than one current snapshot");
    }
    return snapshot_id;
}

void update_session_timestamp(
    sqlite::Database& database, std::string_view session_id,
    std::string_view updated_at) {
    auto update = database.prepare(
        "UPDATE sessions SET updated_at = ?1 WHERE session_id = ?2");
    update.bind_text(1, updated_at);
    update.bind_text(2, session_id);
    step_done(update, "cannot update the session timestamp");
}

model_io::Content decode_content_row(
    const sqlite::Statement& statement, int type_column, int modality_column,
    int raw_column, int extras_column) {
    model_io::Content content;
    content.type =
        enum_value<model_io::ContentType>(
            statement.column_text(type_column), "content.type");
    content.modality =
        enum_value<model_io::Modality>(
            statement.column_text(modality_column), "content.modality");
    content.raw = statement.column_text(raw_column);
    content.extras = decode_optional_json(
        optional_column(statement, extras_column), "content.extras_json");
    return content;
}

void decode_contents(
    sqlite::Database& database, std::int64_t message_id, const char* slot,
    std::vector<model_io::Content>& destination, bool singleton) {
    auto query = database.prepare(
        "SELECT type, modality, raw, extras_json FROM message_contents "
        "WHERE message_id = ?1 AND slot = ?2 ORDER BY ordinal");
    query.bind_int64(1, message_id);
    query.bind_text(2, slot);
    while (query.step() == sqlite::StepResult::Row) {
        destination.push_back(
            decode_content_row(query, 0, 1, 2, 3));
        if (singleton && destination.size() > 1) {
            corrupt(
                std::string("message has more than one content row for slot '")
                + slot + "'");
        }
    }
}

std::optional<model_io::Content> decode_single_content(
    sqlite::Database& database, std::int64_t message_id, const char* slot) {
    std::vector<model_io::Content> contents;
    decode_contents(database, message_id, slot, contents, true);
    if (contents.empty()) return std::nullopt;
    return std::move(contents.front());
}

model_io::InvokeQuery decode_invoke_row(
    const sqlite::Statement& statement, int type_column, int security_column,
    int id_column, int name_column, int arguments_column,
    int extras_column) {
    model_io::InvokeQuery query;
    query.type = enum_value<model_io::InvokeType>(
        statement.column_text(type_column), "invoke.type");
    query.security = enum_value<model_io::InvokeSecurity>(
        statement.column_text(security_column), "invoke.security");
    query.id = statement.column_text(id_column);
    query.name = statement.column_text(name_column);
    query.arguments = parse_json(
        statement.column_text(arguments_column), "invoke.arguments_json");
    query.extras = decode_optional_json(
        optional_column(statement, extras_column), "invoke.extras_json");
    return query;
}

model_io::InvokeReturn decode_invoke_return_row(
    const sqlite::Statement& statement, int query_type_column,
    int query_security_column, int id_column, int name_column,
    int arguments_column, int query_extras_column, int output_type_column,
    int output_modality_column, int output_raw_column,
    int output_extras_column, int extras_column) {
    model_io::InvokeReturn result;
    result.query = decode_invoke_row(
        statement, query_type_column, query_security_column, id_column,
        name_column, arguments_column, query_extras_column);
    result.output = decode_content_row(
        statement, output_type_column, output_modality_column, output_raw_column,
        output_extras_column);
    result.extras = decode_optional_json(
        optional_column(statement, extras_column),
        "invoke_return.extras_json");
    return result;
}

model_io::MessageItem decode_message(
    sqlite::Database& database, std::int64_t message_id) {
    model_io::MessageItem message;
    auto query = database.prepare(
        "SELECT type, role, invokes_present, extras_json FROM messages "
        "WHERE message_id = ?1");
    query.bind_int64(1, message_id);
    if (query.step() != sqlite::StepResult::Row) {
        corrupt("snapshot references a missing message row");
    }
    message.type = enum_value<model_io::MessageItemType>(
        query.column_text(0), "message.type");
    message.role = query.column_text(1);
    const bool invokes_present = query.column_int64(2) != 0;
    message.extras = decode_optional_json(
        optional_column(query, 3), "message.extras_json");

    decode_contents(database, message_id, "body", message.content, false);
    message.reasoning = decode_single_content(database, message_id, "reasoning");
    message.action_status =
        decode_single_content(database, message_id, "action_status");

    {
        auto invokes = database.prepare(
            "SELECT query_type, query_security, wire_call_id, name, "
            "arguments_json, extras_json FROM message_invokes "
            "WHERE message_id = ?1 ORDER BY ordinal");
        invokes.bind_int64(1, message_id);
        std::vector<model_io::InvokeQuery> rows;
        while (invokes.step() == sqlite::StepResult::Row) {
            rows.push_back(decode_invoke_row(invokes, 0, 1, 2, 3, 4, 5));
        }
        if (invokes_present) {
            message.invokes = std::move(rows);
        } else if (!rows.empty()) {
            corrupt("message has invoke rows but invokes_present is zero");
        }
    }

    {
        auto cost = database.prepare(
            "SELECT prompt_u64, generated_u64, cache_hit_u64 "
            "FROM message_costs WHERE message_id = ?1");
        cost.bind_int64(1, message_id);
        if (cost.step() == sqlite::StepResult::Row) {
            model_io::TokenCost value;
            value.prompt = parse_u64(cost.column_text(0), "message_costs.prompt_u64");
            value.generated =
                parse_u64(cost.column_text(1), "message_costs.generated_u64");
            value.cache_hit =
                parse_u64(cost.column_text(2), "message_costs.cache_hit_u64");
            message.cost = value;
        }
    }

    {
        auto result = database.prepare(
            "SELECT query_type, query_security, wire_call_id, query_name, "
            "query_arguments_json, query_extras_json, output_type, "
            "output_modality, output_raw, output_extras_json, extras_json "
            "FROM message_invoke_returns WHERE message_id = ?1");
        result.bind_int64(1, message_id);
        if (result.step() == sqlite::StepResult::Row) {
            message.invoke_return =
                decode_invoke_return_row(result, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
        }
    }
    return message;
}

std::int64_t message_for_slot(
    sqlite::Database& database, const char* column, std::int64_t owner_id,
    const char* slot) {
    const std::string sql =
        std::string("SELECT message_id FROM messages WHERE ") + column
        + " = ?1 AND slot = ?2";
    auto query = database.prepare(sql);
    query.bind_int64(1, owner_id);
    query.bind_text(2, slot);
    if (query.step() != sqlite::StepResult::Row) {
        corrupt(std::string("message slot '") + slot + "' is missing");
    }
    const auto message_id = query.column_int64(0);
    if (query.step() == sqlite::StepResult::Row) {
        corrupt(std::string("message slot '") + slot + "' is not unique");
    }
    return message_id;
}

std::vector<model_io::MessageItem> decode_invoke_returns(
    sqlite::Database& database, std::int64_t step_id) {
    auto query = database.prepare(
        "SELECT message_id FROM messages "
        "WHERE step_id = ?1 AND slot = 'invoke_return' ORDER BY ordinal");
    query.bind_int64(1, step_id);
    std::vector<model_io::MessageItem> results;
    while (query.step() == sqlite::StepResult::Row) {
        results.push_back(decode_message(database, query.column_int64(0)));
    }
    return results;
}

std::vector<model_io::UserLoopStep> decode_turns(
    sqlite::Database& database, std::int64_t snapshot_id) {
    auto query = database.prepare(
        "SELECT turn_id, retain_priority, extras_json FROM turns "
        "WHERE snapshot_id = ?1 ORDER BY ordinal");
    query.bind_int64(1, snapshot_id);
    std::vector<model_io::UserLoopStep> turns;
    while (query.step() == sqlite::StepResult::Row) {
        model_io::UserLoopStep turn;
        const auto turn_id = query.column_int64(0);
        turn.retain_priority = enum_value<model_io::RetainPriority>(
            query.column_text(1), "turns.retain_priority");
        turn.extras = decode_optional_json(
            optional_column(query, 2), "turns.extras_json");
        turn.user_input = decode_message(
            database,
            message_for_slot(database, "turn_id", turn_id, "user_input"));

        auto steps = database.prepare(
            "SELECT step_id, retain_priority, commit_sequence_u64, "
            "invoke_returns_present, extras_json FROM agent_steps "
            "WHERE turn_id = ?1 ORDER BY ordinal");
        steps.bind_int64(1, turn_id);
        while (steps.step() == sqlite::StepResult::Row) {
            model_io::AgentLoopStep step;
            const auto step_id = steps.column_int64(0);
            step.retain_priority = enum_value<model_io::RetainPriority>(
                steps.column_text(1), "agent_steps.retain_priority");
            step.commit_sequence = parse_u64(
                steps.column_text(2), "agent_steps.commit_sequence_u64");
            const bool returns_present = steps.column_int64(3) != 0;
            step.extras = decode_optional_json(
                optional_column(steps, 4), "agent_steps.extras_json");
            step.model_response = decode_message(
                database,
                message_for_slot(
                    database, "step_id", step_id, "model_response"));
            auto results = decode_invoke_returns(database, step_id);
            if (returns_present) {
                step.invoke_returns = std::move(results);
            } else if (!results.empty()) {
                corrupt(
                    "agent step has invoke result rows but "
                    "invoke_returns_present is zero");
            }
            turn.agent_loop_step.push_back(std::move(step));
        }
        turns.push_back(std::move(turn));
    }
    return turns;
}

std::optional<model_io::LoopProgress> decode_loop(
    sqlite::Database& database, std::int64_t snapshot_id) {
    auto query = database.prepare(
        "SELECT status, phase, completed_exchanges_u64, "
        "committed_response_sequence_u64, error FROM loop_progress "
        "WHERE snapshot_id = ?1");
    query.bind_int64(1, snapshot_id);
    if (query.step() != sqlite::StepResult::Row) return std::nullopt;
    model_io::LoopProgress loop;
    loop.status = enum_value<model_io::LoopStatus>(
        query.column_text(0), "loop_progress.status");
    loop.phase = enum_value<model_io::LoopPhase>(
        query.column_text(1), "loop_progress.phase");
    const auto completed = parse_u64(
        query.column_text(2), "loop_progress.completed_exchanges_u64");
    if (completed > std::numeric_limits<std::size_t>::max()) {
        corrupt("loop_progress.completed_exchanges_u64 exceeds size_t");
    }
    loop.completed_exchanges = static_cast<std::size_t>(completed);
    loop.committed_response_sequence = parse_u64(
        query.column_text(3),
        "loop_progress.committed_response_sequence_u64");
    loop.error = query.column_text(4);

    auto pending = database.prepare(
        "SELECT query_type, query_security, wire_call_id, query_name, "
        "query_arguments_json, query_extras_json, output_type, "
        "output_modality, output_raw, output_extras_json, extras_json "
        "FROM pending_results WHERE snapshot_id = ?1 ORDER BY ordinal");
    pending.bind_int64(1, snapshot_id);
    while (pending.step() == sqlite::StepResult::Row) {
        loop.pending_results.push_back(
            decode_invoke_return_row(pending, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    }
    return loop;
}

std::vector<model_io::Invocable> decode_tools(
    sqlite::Database& database, std::int64_t snapshot_id) {
    auto query = database.prepare(
        "SELECT name, description, argument_schema_json, remote_type, "
        "extras_json FROM tool_definitions "
        "WHERE snapshot_id = ?1 ORDER BY ordinal");
    query.bind_int64(1, snapshot_id);
    std::vector<model_io::Invocable> tools;
    while (query.step() == sqlite::StepResult::Row) {
        model_io::Invocable tool;
        tool.name = query.column_text(0);
        tool.description = query.column_text(1);
        tool.argument_schema = parse_json(
            query.column_text(2), "tool_definitions.argument_schema_json");
        tool.remote_type = optional_column(query, 3);
        tool.extras = decode_optional_json(
            optional_column(query, 4), "tool_definitions.extras_json");
        tools.push_back(std::move(tool));
    }
    return tools;
}

model_io::PromptTemplate decode_prompt(
    sqlite::Database& database, std::int64_t snapshot_id,
    std::int64_t heading_level) {
    if (heading_level < std::numeric_limits<int>::min()
        || heading_level > std::numeric_limits<int>::max()) {
        corrupt("snapshots.heading_level does not fit int");
    }
    model_io::PromptTemplate prompt;
    prompt.heading_level = static_cast<int>(heading_level);
    auto query = database.prepare(
        "SELECT name, title, stability, body FROM prompt_sections "
        "WHERE snapshot_id = ?1 ORDER BY ordinal");
    query.bind_int64(1, snapshot_id);
    while (query.step() == sqlite::StepResult::Row) {
        try {
            prompt.add_section(
                query.column_text(0), query.column_text(1),
                query.column_text(3),
                enum_value<model_io::SectionStability>(
                    query.column_text(2), "prompt_sections.stability"));
        } catch (const std::logic_error& error) {
            corrupt(
                std::string("snapshot has an invalid prompt layout: ")
                + error.what());
        }
    }
    return prompt;
}

model_io::AgentInputState decode_snapshot_impl(
    sqlite::Database& database, std::int64_t snapshot_id) {
    auto header = database.prepare(
        "SELECT session_id, meta_schema_version, session_status, "
        "meta_error_json, created_at, updated_at, heading_level, extras_json "
        "FROM snapshots WHERE snapshot_id = ?1");
    header.bind_int64(1, snapshot_id);
    if (header.step() != sqlite::StepResult::Row) {
        throw SnapshotError(
            Reason::SnapshotNotFound,
            "session store has no snapshot " + std::to_string(snapshot_id));
    }

    model_io::AgentInputState state;
    state.meta.session_id = header.column_text(0);
    const auto schema_version = header.column_int64(1);
    if (schema_version < 0
        || schema_version
            > std::numeric_limits<std::uint32_t>::max()) {
        corrupt("snapshots.meta_schema_version does not fit uint32");
    }
    state.meta.schema_version = static_cast<std::uint32_t>(schema_version);
    state.meta.status = enum_value<model_io::SessionStatus>(
        header.column_text(2), "snapshots.session_status");
    state.meta.error = decode_optional_json(
        optional_column(header, 3), "snapshots.meta_error_json");
    state.meta.created_at = header.column_text(4);
    state.meta.updated_at = header.column_text(5);
    state.system_prompt =
        decode_prompt(database, snapshot_id, header.column_int64(6));
    state.extras = decode_optional_json(
        optional_column(header, 7), "snapshots.extras_json");

    state.tools = decode_tools(database, snapshot_id);
    state.turns = decode_turns(database, snapshot_id);
    state.loop = decode_loop(database, snapshot_id);
    return state;
}

} // namespace

SnapshotError::SnapshotError(Reason reason, const std::string& message)
    : std::runtime_error(message), reason_(reason) {}

SnapshotError::~SnapshotError() = default;

std::int64_t encode_snapshot(
    sqlite::Database& database, std::string_view session_id,
    const model_io::AgentInputState& state, SnapshotKind kind, bool own_transaction) {
    require_matching_session(session_id, state.meta);
    require_foreign_keys(database);
    std::unique_ptr<sqlite::Transaction> transaction;
    if (own_transaction) transaction = std::make_unique<sqlite::Transaction>(database);
    require_session(database, session_id);
    const auto snapshot_id =
        insert_snapshot_header(database, session_id, state, kind, std::nullopt);
    insert_graph(database, snapshot_id, state);
    if (transaction) transaction->commit();
    return snapshot_id;
}

std::int64_t store_archive_snapshot(
    sqlite::Database& database, std::string_view session_id,
    const model_io::AgentInputState& state, bool own_transaction) {
    return encode_snapshot(
        database, session_id, state, SnapshotKind::Archive, own_transaction);
}

std::int64_t replace_current(
    sqlite::Database& database, std::string_view session_id,
    const model_io::AgentInputState& state, bool own_transaction) {
    require_matching_session(session_id, state.meta);
    require_foreign_keys(database);
    std::unique_ptr<sqlite::Transaction> transaction;
    if (own_transaction) transaction = std::make_unique<sqlite::Transaction>(database);
    require_session(database, session_id);

    const auto current = find_current(database, session_id);
    std::int64_t snapshot_id = 0;
    if (current) {
        // One transaction replaces the complete graph: deleting the header
        // cascades every child row, then the header is reinserted under the
        // same identity (D6, D9).
        auto remove = database.prepare(
            "DELETE FROM snapshots WHERE snapshot_id = ?1");
        remove.bind_int64(1, *current);
        step_done(remove, "cannot clear the current snapshot");
        snapshot_id = insert_snapshot_header(
            database, session_id, state, SnapshotKind::Current, *current);
    } else {
        snapshot_id = insert_snapshot_header(
            database, session_id, state, SnapshotKind::Current, std::nullopt);
    }
    insert_graph(database, snapshot_id, state);
    update_session_timestamp(database, session_id, state.meta.updated_at);
    if (transaction) transaction->commit();
    return snapshot_id;
}

std::optional<model_io::AgentInputState> load_current(
    sqlite::Database& database, std::string_view session_id, bool own_transaction) {
    std::unique_ptr<sqlite::Transaction> transaction;
    if (own_transaction) transaction = std::make_unique<sqlite::Transaction>(database);
    require_session(database, session_id);
    const auto current = find_current(database, session_id);
    if (!current) return std::nullopt;
    auto state = decode_snapshot_impl(database, *current);
    if (transaction) transaction->rollback();
    return state;
}

model_io::AgentInputState decode_snapshot(
    sqlite::Database& database, std::int64_t snapshot_id) {
    sqlite::Transaction transaction(database);
    auto state = decode_snapshot_impl(database, snapshot_id);
    transaction.rollback();
    return state;
}

} // namespace persistence::sqlite_snapshot
