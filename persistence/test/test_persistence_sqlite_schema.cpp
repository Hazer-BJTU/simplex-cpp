#define BOOST_TEST_MODULE PersistenceSQLiteSchema
#include <boost/test/unit_test.hpp>

#include "sqlite/database.hpp"
#include "sqlite_schema.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using persistence::sqlite_schema::Migration;
using persistence::sqlite_schema::Reason;
using persistence::sqlite_schema::SchemaError;

namespace {

/** Give each test a private disk tree without modifying the working directory. */
struct Scratch {
    fs::path root = fs::temp_directory_path()
        / ("simplex_sqlite_schema_" + std::to_string(::getpid()));

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

std::int64_t scalar(sqlite::Database& database, std::string_view sql) {
    auto query = database.prepare(sql);
    BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
    return query.column_int64(0);
}

std::string text(sqlite::Database& database, std::string_view sql) {
    auto query = database.prepare(sql);
    BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
    return query.column_text(0);
}

bool has_table(sqlite::Database& database, std::string_view name) {
    auto query = database.prepare(
        "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = ?1");
    query.bind_text(1, name);
    BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
    return query.column_int64(0) == 1;
}

template<typename Function>
void expect_schema_error(Function&& function, Reason expected) {
    try {
        function();
        BOOST_FAIL("expected SchemaError");
    } catch (const SchemaError& error) {
        BOOST_TEST(code(error.reason()) == code(expected));
    }
}

} // namespace

BOOST_AUTO_TEST_CASE(empty_database_becomes_a_complete_store) {
    Scratch scratch;
    sqlite::Database database(scratch.database());
    const auto identity = persistence::sqlite_schema::open_store(
        database, "session-1", "2026-10-18T00:00:00Z");

    BOOST_TEST(identity.session_id == "session-1");
    BOOST_TEST(identity.store_id.size() == 32);
    BOOST_TEST(identity.store_id.find_first_not_of("0123456789abcdef")
               == std::string::npos);
    BOOST_TEST(identity.schema_version == persistence::sqlite_schema::schema_version);
    BOOST_TEST(identity.codec_version == persistence::sqlite_schema::codec_version);

    BOOST_TEST(
        scalar(database, "PRAGMA application_id")
        == persistence::sqlite_schema::application_id);
    BOOST_TEST(scalar(database, "PRAGMA foreign_keys") == 1);
    BOOST_TEST(text(database, "PRAGMA journal_mode") == "wal");
    BOOST_TEST(scalar(database, "PRAGMA synchronous") == 2); // FULL

    BOOST_TEST(scalar(database, "SELECT count(*) FROM store_meta") == 1);
    BOOST_TEST(scalar(database, "SELECT count(*) FROM sessions") == 1);
    BOOST_TEST(scalar(database, "SELECT count(*) FROM schema_migrations") == 1);
    BOOST_TEST(scalar(database, "SELECT version FROM schema_migrations") == 1);
    BOOST_TEST(
        text(database, "SELECT created_at FROM sessions") == "2026-10-18T00:00:00Z");
    BOOST_TEST(
        text(database, "SELECT updated_at FROM sessions") == "2026-10-18T00:00:00Z");

    const std::vector<std::string_view> tables = {
        "store_meta",         "schema_migrations",   "sessions",
        "snapshots",          "prompt_sections",     "tool_definitions",
        "turns",              "agent_steps",         "messages",
        "message_contents",   "message_costs",       "message_invokes",
        "message_invoke_returns", "loop_progress",   "pending_results",
        "archives",
    };
    for (const auto table : tables) {
        BOOST_TEST_INFO("table " << table);
        BOOST_TEST(has_table(database, table));
    }
    BOOST_TEST(text(database, "PRAGMA integrity_check") == "ok");

    // Reopening is idempotent: the same store identity, no duplicate rows.
    const auto reopened = persistence::sqlite_schema::open_store(
        database, "session-1", "2026-10-18T00:00:01Z");
    BOOST_TEST(reopened.store_id == identity.store_id);
    BOOST_TEST(reopened.session_id == identity.session_id);
    BOOST_TEST(scalar(database, "SELECT count(*) FROM store_meta") == 1);
    BOOST_TEST(scalar(database, "SELECT count(*) FROM sessions") == 1);
    BOOST_TEST(scalar(database, "SELECT count(*) FROM schema_migrations") == 1);
}

BOOST_AUTO_TEST_CASE(read_only_inspection_never_creates_or_writes) {
    Scratch scratch;
    const auto path = scratch.database();
    {
        sqlite::Database writer(path);
        persistence::sqlite_schema::open_store(
            writer, "session-1", "2026-10-18T00:00:00Z");
        writer.execute("CREATE TABLE sentinel(value TEXT)");
        writer.execute("INSERT INTO sentinel VALUES('unchanged')");
    }

    {
        sqlite::Database reader(path, sqlite::OpenMode::ReadOnly);
        const auto identity = persistence::sqlite_schema::inspect_store(
            reader, "session-1");
        BOOST_TEST(identity.session_id == "session-1");
        BOOST_TEST(identity.schema_version == 1);
        BOOST_TEST(text(reader, "SELECT value FROM sentinel") == "unchanged");
        BOOST_CHECK_THROW(
            reader.execute("CREATE TABLE forbidden(value TEXT)"), sqlite::Error);
        BOOST_CHECK_THROW(
            reader.execute("INSERT INTO sentinel VALUES('forbidden')"),
            sqlite::Error);
    }

    // A missing file is not fabricated by a read-only open.
    const fs::path missing = scratch.root / "absent.sqlite3";
    BOOST_CHECK_THROW(
        sqlite::Database(missing.string(), sqlite::OpenMode::ReadOnly),
        sqlite::Error);
    BOOST_TEST(!fs::exists(missing));

    sqlite::Database writer(path);
    const auto identity = persistence::sqlite_schema::inspect_store(
        writer, "session-1");
    BOOST_TEST(identity.store_id.size() == 32);
    BOOST_TEST(scalar(writer, "SELECT count(*) FROM sentinel") == 1);
}

BOOST_AUTO_TEST_CASE(future_schema_and_upgrade_requirement_are_refused) {
    Scratch scratch;
    const auto path = scratch.database();
    {
        sqlite::Database database(path);
        persistence::sqlite_schema::open_store(
            database, "session-1", "2026-10-18T00:00:00Z");
        database.execute("UPDATE store_meta SET store_schema_version = 2");
    }

    {
        sqlite::Database database(path);
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::open_store(
                    database, "session-1", "2026-10-18T00:00:00Z");
            },
            Reason::NewerSchema);
        BOOST_TEST(
            scalar(database, "SELECT store_schema_version FROM store_meta") == 2);
        BOOST_TEST(scalar(database, "SELECT count(*) FROM schema_migrations") == 1);
    }
    {
        sqlite::Database database(path);
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::inspect_store(database, "session-1");
            },
            Reason::NewerSchema);
    }

    {
        sqlite::Database database(path);
        database.execute("UPDATE store_meta SET store_schema_version = 0");
    }
    {
        sqlite::Database database(path, sqlite::OpenMode::ReadOnly);
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::inspect_store(database, "session-1");
            },
            Reason::UpgradeRequired);
    }
    {
        sqlite::Database database(path);
        BOOST_TEST(
            scalar(database, "SELECT store_schema_version FROM store_meta") == 0);
        BOOST_TEST(scalar(database, "SELECT count(*) FROM schema_migrations") == 1);
    }
}

BOOST_AUTO_TEST_CASE(foreign_incomplete_and_mismatched_databases_are_refused) {
    {
        sqlite::Database database(":memory:");
        database.execute("CREATE TABLE unrelated(value TEXT)");
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::open_store(
                    database, "session-1", "2026-10-18T00:00:00Z");
            },
            Reason::ForeignDatabase);
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::inspect_store(database, "session-1");
            },
            Reason::ForeignDatabase);
    }
    {
        sqlite::Database database(":memory:");
        database.execute("PRAGMA application_id = 12345");
        database.execute("CREATE TABLE unrelated(value TEXT)");
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::open_store(
                    database, "session-1", "2026-10-18T00:00:00Z");
            },
            Reason::ForeignDatabase);
    }
    {
        // A file that claims the Simplex application id but has no metadata.
        sqlite::Database database(":memory:");
        database.execute(
            "PRAGMA application_id = "
            + std::to_string(persistence::sqlite_schema::application_id));
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::open_store(
                    database, "session-1", "2026-10-18T00:00:00Z");
            },
            Reason::IncompleteSchema);
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::inspect_store(database, "session-1");
            },
            Reason::IncompleteSchema);
    }
    {
        sqlite::Database database(":memory:");
        persistence::sqlite_schema::open_store(
            database, "session-1", "2026-10-18T00:00:00Z");
        database.execute("DELETE FROM sessions");
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::open_store(
                    database, "session-1", "2026-10-18T00:00:00Z");
            },
            Reason::IncompleteSchema);
    }
    {
        sqlite::Database database(":memory:");
        persistence::sqlite_schema::open_store(
            database, "session-1", "2026-10-18T00:00:00Z");
        database.execute("DROP INDEX snapshots_one_current");
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::open_store(
                    database, "session-1", "2026-10-18T00:00:00Z");
            },
            Reason::IncompleteSchema);
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::inspect_store(database, "session-1");
            },
            Reason::IncompleteSchema);
    }
    {
        sqlite::Database database(":memory:");
        persistence::sqlite_schema::open_store(
            database, "session-1", "2026-10-18T00:00:00Z");
        database.execute("DROP TABLE pending_results");
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::open_store(
                    database, "session-1", "2026-10-18T00:00:00Z");
            },
            Reason::IncompleteSchema);
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::inspect_store(database, "session-1");
            },
            Reason::IncompleteSchema);
    }
    {
        sqlite::Database database(":memory:");
        persistence::sqlite_schema::open_store(
            database, "session-1", "2026-10-18T00:00:00Z");
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::open_store(
                    database, "session-2", "2026-10-18T00:00:00Z");
            },
            Reason::SessionMismatch);
        expect_schema_error(
            [&] {
                persistence::sqlite_schema::inspect_store(database, "session-2");
            },
            Reason::SessionMismatch);
        BOOST_TEST(text(database, "SELECT session_id FROM sessions") == "session-1");
    }
}

BOOST_AUTO_TEST_CASE(migrations_record_and_roll_back_as_one_transaction) {
    sqlite::Database database(":memory:");
    database.execute(
        "CREATE TABLE schema_migrations("
        "version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");

    persistence::sqlite_schema::apply_migrations(
        database, 0,
        {{1, "CREATE TABLE alpha(value TEXT) STRICT;"},
         {2, "CREATE TABLE beta(value TEXT) STRICT;"}},
        "2026-10-18T00:00:00Z");
    BOOST_TEST(has_table(database, "alpha"));
    BOOST_TEST(has_table(database, "beta"));
    BOOST_TEST(scalar(database, "SELECT count(*) FROM schema_migrations") == 2);
    BOOST_TEST(scalar(database, "SELECT max(version) FROM schema_migrations") == 2);

    // Only migrations newer than the recorded version run.
    persistence::sqlite_schema::apply_migrations(
        database, 2,
        {{1, "CREATE TABLE alpha(value TEXT) STRICT;"},
         {2, "CREATE TABLE beta(value TEXT) STRICT;"},
         {3, "CREATE TABLE gamma(value TEXT) STRICT;"}},
        "2026-10-18T00:00:01Z");
    BOOST_TEST(has_table(database, "gamma"));
    BOOST_TEST(scalar(database, "SELECT count(*) FROM schema_migrations") == 3);
}

BOOST_AUTO_TEST_CASE(failing_migration_leaves_schema_and_versions_unchanged) {
    sqlite::Database database(":memory:");
    database.execute(
        "CREATE TABLE schema_migrations("
        "version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)");

    const std::vector<Migration> migrations = {
        {1, "CREATE TABLE alpha(value TEXT) STRICT;"},
        {2, "CREATE TABLE beta(value TEXT) STRICT;"},
        {3, "CREATE TABLE gamma(value TEXT) STRICT; THIS IS NOT SQL"},
    };
    BOOST_CHECK_THROW(
        persistence::sqlite_schema::apply_migrations(
            database, 0, migrations, "2026-10-18T00:00:00Z"),
        sqlite::Error);

    BOOST_TEST(!has_table(database, "alpha"));
    BOOST_TEST(!has_table(database, "beta"));
    BOOST_TEST(!has_table(database, "gamma"));
    BOOST_TEST(scalar(database, "SELECT count(*) FROM schema_migrations") == 0);

    // The connection is still usable for a successful migration afterwards.
    persistence::sqlite_schema::apply_migrations(
        database, 0, {{1, "CREATE TABLE delta(value TEXT) STRICT;"}},
        "2026-10-18T00:00:01Z");
    BOOST_TEST(has_table(database, "delta"));
    BOOST_TEST(scalar(database, "SELECT count(*) FROM schema_migrations") == 1);
}

BOOST_AUTO_TEST_CASE(optional_access_indexes_are_writer_only_and_keep_v1_readable) {
    Scratch scratch;
    {
        sqlite::Database database(scratch.database());
        persistence::sqlite_schema::open_store(database, "session-1", "now");
        database.execute("DROP INDEX messages_turn_owner; DROP INDEX messages_step_owner;");
    }
    {
        sqlite::Database database(scratch.database(), sqlite::OpenMode::ReadOnly);
        const auto identity = persistence::sqlite_schema::inspect_store(database, "session-1");
        BOOST_TEST(identity.schema_version == 1);
        BOOST_TEST(scalar(database,
            "SELECT count(*) FROM sqlite_master WHERE name IN "
            "('messages_turn_owner','messages_step_owner')") == 0);
    }
    {
        sqlite::Database database(scratch.database());
        const auto identity = persistence::sqlite_schema::open_store(database, "session-1", "later");
        BOOST_TEST(identity.schema_version == 1);
        BOOST_TEST(scalar(database, "SELECT count(*) FROM schema_migrations") == 1);
        BOOST_TEST(scalar(database,
            "SELECT count(*) FROM sqlite_master WHERE name IN "
            "('messages_turn_owner','messages_step_owner')") == 2);

        // Assert indexed searches rather than machine-dependent time limits.
        // These are the lookups performed by FK cascades and snapshot decoding.
        for (const auto* sql : {
            "EXPLAIN QUERY PLAN SELECT rowid FROM messages WHERE snapshot_id = 1 AND turn_id = 2",
            "EXPLAIN QUERY PLAN SELECT rowid FROM messages WHERE snapshot_id = 1 AND step_id = 2",
            "EXPLAIN QUERY PLAN SELECT message_id FROM messages WHERE step_id = 1 "
                "AND slot = 'model_response' AND slot <> 'user_input'",
            "EXPLAIN QUERY PLAN SELECT message_id FROM messages WHERE step_id = 1 "
                "AND slot = 'invoke_return' AND slot <> 'user_input' ORDER BY ordinal"
        }) {
            auto query = database.prepare(sql);
            BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
            BOOST_TEST(query.column_text(3).find("SEARCH messages") != std::string::npos);
        }
    }
}
