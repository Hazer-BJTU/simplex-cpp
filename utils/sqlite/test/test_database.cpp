#define BOOST_TEST_MODULE SQLiteOwnership
#include <boost/test/unit_test.hpp>

#include "sqlite/database.hpp"
#include <sqlite3.h>

#include <boost/filesystem.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>

namespace {

/** Unique on-disk fixture, shared by independent connections within one test. */
struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("simplex-sqlite-%%%%-%%%%-%%%%").string();

    Scratch() {
        std::filesystem::create_directory(root);
    }

    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::string database() const {
        return (root / "state.sqlite").string();
    }
};

std::int64_t count(sqlite::Database& database) {
    auto query = database.prepare("SELECT count(*) FROM items");
    BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
    return query.column_int64(0);
}

} // namespace

BOOST_AUTO_TEST_CASE(pinned_runtime_and_options) {
    BOOST_TEST(std::string(sqlite3_libversion()) == "3.54.0");
    BOOST_TEST(std::string(sqlite3_sourceid()) == SQLITE_SOURCE_ID);
    BOOST_TEST(sqlite3_threadsafe() == 1);
    BOOST_TEST(sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") == 1);
    static_assert(!std::is_copy_constructible_v<sqlite::Database>);
    static_assert(!std::is_move_constructible_v<sqlite::Statement>);
    static_assert(!std::is_copy_constructible_v<sqlite::Transaction>);
    static_assert(std::is_nothrow_destructible_v<sqlite::Transaction>);
}

BOOST_AUTO_TEST_CASE(bindings_copy_text_preserve_nul_and_distinguish_null) {
    sqlite::Database database(":memory:");
    database.execute("CREATE TABLE items(id INTEGER, value TEXT, empty TEXT, missing TEXT)");
    auto insert = database.prepare("INSERT INTO items VALUES(?1, ?2, ?3, ?4)");
    const std::string expected("hello\0\xc3\xa9", 8);
    {
        std::string temporary = expected;
        insert.bind_int64(1, 9223372036854775807LL);
        insert.bind_text(2, temporary);
        insert.bind_text(3, {});
        insert.bind_null(4);
        temporary.assign("changed after binding");
    }
    BOOST_CHECK(insert.step() == sqlite::StepResult::Done);
    BOOST_CHECK(insert.step() == sqlite::StepResult::Done);
    BOOST_TEST(count(database) == 1); // No automatic rerun after SQLITE_DONE.
    auto select = database.prepare("SELECT id, value, empty, missing FROM items; \n");
    BOOST_CHECK_THROW(select.column_text(0), std::logic_error);
    BOOST_REQUIRE(select.step() == sqlite::StepResult::Row);
    BOOST_TEST(select.column_int64(0) == 9223372036854775807LL);
    const auto saved = select.column_text(1);
    BOOST_TEST(saved == expected);
    BOOST_TEST(!select.column_is_null(2));
    BOOST_TEST(select.column_text(2).empty());
    BOOST_TEST(select.column_is_null(3));
    BOOST_CHECK_THROW(select.column_text(-1), std::out_of_range);
    BOOST_CHECK_THROW(select.column_text(4), std::out_of_range);
    BOOST_CHECK(select.step() == sqlite::StepResult::Done);
    BOOST_CHECK_THROW(select.column_text(0), std::logic_error);
    BOOST_TEST(saved == expected);
}

BOOST_AUTO_TEST_CASE(validation_and_failed_statement_do_not_poison_connection) {
    sqlite::Database database(":memory:");
    BOOST_CHECK_THROW(sqlite::Database(""), std::invalid_argument);
    BOOST_CHECK_THROW(database.prepare(""), std::invalid_argument);
    BOOST_CHECK_THROW(database.prepare(" -- only a comment"), std::invalid_argument);
    BOOST_CHECK_THROW(database.prepare("SELECT 1; SELECT 2"), std::invalid_argument);
    BOOST_CHECK_THROW(database.prepare("SELECT 1; -- trailing comment"), std::invalid_argument);
    BOOST_CHECK_THROW(database.execute(std::string("SELECT 1\0; SELECT 2", 19)), std::invalid_argument);
    database.execute("CREATE TABLE items(value TEXT UNIQUE)");
    database.execute("INSERT INTO items VALUES('secret-value')");
    auto insert = database.prepare("INSERT INTO items VALUES(?1)");
    BOOST_CHECK_THROW(insert.bind_text(0, "x"), std::out_of_range);
    BOOST_CHECK_THROW(insert.bind_text(2, "x"), std::out_of_range);
    insert.bind_text(1, "secret-value");
    try {
        insert.step();
        BOOST_FAIL("duplicate must fail");
    } catch (const sqlite::Error& error) {
        BOOST_TEST(error.code() == SQLITE_CONSTRAINT);
        BOOST_TEST(error.extended_code() == SQLITE_CONSTRAINT_UNIQUE);
        BOOST_TEST(std::string(error.what()).find("secret-value") == std::string::npos);
    }
    BOOST_CHECK_THROW(insert.step(), std::logic_error);
    BOOST_CHECK_THROW(insert.bind_text(1, "x"), std::logic_error);
    BOOST_TEST(count(database) == 1);
    BOOST_CHECK_THROW(database.prepare("invalid SQL"), sqlite::Error);
    BOOST_TEST(count(database) == 1);
}

BOOST_AUTO_TEST_CASE(transaction_commit_rollback_and_exception_unwinding) {
    sqlite::Database database(":memory:");
    database.execute("CREATE TABLE items(value TEXT)");
    {
        sqlite::Transaction transaction(database);
        BOOST_CHECK_THROW(sqlite::Transaction nested(database), std::logic_error);
        database.execute("INSERT INTO items VALUES('committed')");
        transaction.commit();
        BOOST_CHECK_THROW(transaction.commit(), std::logic_error);
    }
    {
        sqlite::Transaction transaction(database);
        database.execute("INSERT INTO items VALUES('rolled back')");
        transaction.rollback();
    }
    try {
        sqlite::Transaction transaction(database);
        database.execute("INSERT INTO items VALUES('unwound')");
        database.execute("INSERT INTO nonexistent VALUES(1)");
    } catch (const sqlite::Error&) {}
    BOOST_TEST(count(database) == 1);
    database.execute("BEGIN");
    BOOST_CHECK_THROW(sqlite::Transaction nested(database), std::logic_error);
    database.execute("ROLLBACK");
    sqlite::Transaction next(database);
    next.commit();
}

BOOST_AUTO_TEST_CASE(automatic_sqlite_rollback_does_not_report_commit_success) {
    sqlite::Database database(":memory:");
    database.execute("CREATE TABLE items(value TEXT UNIQUE)");
    {
        sqlite::Transaction transaction(database);
        database.execute("INSERT INTO items VALUES('one')");
        BOOST_CHECK_THROW(database.execute("INSERT OR ROLLBACK INTO items VALUES('one')"), sqlite::Error);
        BOOST_CHECK_THROW(transaction.commit(), sqlite::Error);
        BOOST_CHECK_THROW(sqlite::Transaction nested(database), std::logic_error);
    }
    BOOST_TEST(count(database) == 0);
    sqlite::Transaction next(database);
    next.commit();
}

BOOST_AUTO_TEST_CASE(statements_and_transactions_retain_native_connection) {
    auto database = std::make_unique<sqlite::Database>(":memory:");
    auto query = database->prepare("SELECT 42");
    sqlite::Transaction transaction(*database);
    database.reset();
    BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
    BOOST_TEST(query.column_int64(0) == 42);
    transaction.commit();
}

BOOST_AUTO_TEST_CASE(disk_reopen_and_busy_commit_recovery) {
    Scratch scratch;
    {
        sqlite::Database writer(scratch.database());
        writer.execute("PRAGMA journal_mode=DELETE");
        writer.execute("CREATE TABLE items(value TEXT)");
        writer.execute("INSERT INTO items VALUES('original')");
        sqlite::Database reader(scratch.database());
        sqlite::Transaction read_transaction(reader);
        BOOST_TEST(count(reader) == 1); // Retain a shared lock until rollback.
        {
            sqlite::Transaction write_transaction(writer);
            writer.execute("INSERT INTO items VALUES('blocked commit')");
            try {
                write_transaction.commit();
                BOOST_FAIL("reader must prevent commit in DELETE journal mode");
            } catch (const sqlite::Error& error) {
                BOOST_TEST(error.code() == SQLITE_BUSY);
            }
            // Failed commit is still owned and rolled back by the guard.
        }
        read_transaction.rollback();
        BOOST_TEST(count(writer) == 1);
        sqlite::Transaction successful(writer);
        writer.execute("INSERT INTO items VALUES('persisted')");
        successful.commit();
    }
    sqlite::Database reopened(scratch.database());
    BOOST_TEST(count(reopened) == 2);
    auto check = reopened.prepare("PRAGMA integrity_check");
    BOOST_REQUIRE(check.step() == sqlite::StepResult::Row);
    BOOST_TEST(check.column_text(0) == "ok");
    BOOST_CHECK_THROW(sqlite::Database missing((scratch.root / "absent" / "db").string()), sqlite::Error);
}

BOOST_AUTO_TEST_CASE(read_only_open_never_creates_or_writes) {
    Scratch scratch;
    {
        sqlite::Database writer(scratch.database());
        writer.execute("CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT)");
        writer.execute("INSERT INTO items(value) VALUES('persisted')");
    }
    sqlite::Database reader(scratch.database(), sqlite::OpenMode::ReadOnly);
    BOOST_TEST(count(reader) == 1);
    BOOST_CHECK_THROW(
        reader.execute("INSERT INTO items(value) VALUES('rejected')"), sqlite::Error);
    // Opening a missing path read-only must fail instead of fabricating a file.
    BOOST_CHECK_THROW(
        sqlite::Database missing((scratch.root / "absent.sqlite").string(),
            sqlite::OpenMode::ReadOnly),
        sqlite::Error);
    BOOST_TEST(!std::filesystem::exists(scratch.root / "absent.sqlite"));
}

BOOST_AUTO_TEST_CASE(last_insert_rowid_tracks_the_latest_insert) {
    sqlite::Database database(":memory:");
    BOOST_TEST(database.last_insert_rowid() == 0);
    database.execute("CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT)");
    auto insert = database.prepare("INSERT INTO items(value) VALUES(?1)");
    insert.bind_text(1, "first");
    BOOST_CHECK(insert.step() == sqlite::StepResult::Done);
    const auto first = database.last_insert_rowid();
    BOOST_TEST(first > 0);
    auto second = database.prepare("INSERT INTO items(value) VALUES('second')");
    BOOST_CHECK(second.step() == sqlite::StepResult::Done);
    BOOST_TEST(database.last_insert_rowid() == first + 1);
}
