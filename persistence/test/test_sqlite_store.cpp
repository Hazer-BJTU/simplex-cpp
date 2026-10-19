#define BOOST_TEST_MODULE SqliteStateStore
#include <boost/test/unit_test.hpp>
#include "persistence/store.hpp"
#include "sqlite/database.hpp"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("simplex_sqlite_store_" + std::to_string(getpid()));
    Scratch() { std::filesystem::remove_all(root); std::filesystem::create_directories(root); }
    ~Scratch() { std::filesystem::remove_all(root); }
};
model_io::AgentInputState state() {
    model_io::AgentInputState result;
    result.meta.session_id = "session-1";
    result.system_prompt.add_section("system", "System", "test");
    result.extras = nlohmann::json{{"large", std::string(1024 * 1024, 'x')}};
    result.loop.emplace();
    result.loop->phase = model_io::LoopPhase::Projection;
    return result;
}
}

BOOST_AUTO_TEST_CASE(shared_snapshot_contract) {
    Scratch scratch;
    for (bool sql : {false, true}) {
        const auto directory = scratch.root / (sql ? "sqlite" : "json");
        auto store = sql ? persistence::make_sqlite_store("session-1", directory)
            : persistence::make_json_store("session-1", {directory, scratch.root / "memory"});
        BOOST_TEST(!store->load_current());
        BOOST_TEST(!std::filesystem::exists(directory));
        auto original = state();
        const auto receipt = store->checkpoint(original, persistence::Boundary::ResultsReady);
        BOOST_CHECK(receipt.status == persistence::CommitStatus::Committed);
        auto restored = store->load_current();
        BOOST_REQUIRE(restored);
        BOOST_TEST(nlohmann::json(restored->state) == nlohmann::json(original));
        original.extras.reset();
        original.loop->phase = model_io::LoopPhase::Blocked;
        store->checkpoint(original, persistence::Boundary::RunFinished, receipt.revision);
        BOOST_TEST(nlohmann::json(store->load_current()->state) == nlohmann::json(original));
    }
}

BOOST_AUTO_TEST_CASE(revisions_reopen_and_stale_writes) {
    Scratch scratch;
    auto store = persistence::make_sqlite_store("session-1", scratch.root);
    auto original = state();
    auto first = store->checkpoint(original, persistence::Boundary::BeforeTools, 0);
    BOOST_REQUIRE(first.revision);
    BOOST_TEST(*first.revision == 1u);
    BOOST_TEST(!first.operation_id.empty());
    store.reset();
    store = persistence::make_sqlite_store("session-1", scratch.root);
    auto loaded = store->load_current();
    BOOST_TEST(loaded->operation_id == first.operation_id);
    BOOST_CHECK_THROW(store->checkpoint(original, persistence::Boundary::RunFinished, 0), persistence::StoreError);
    BOOST_TEST(store->load_current()->operation_id == first.operation_id);
    auto second = store->checkpoint(original, persistence::Boundary::RunFinished, 1);
    BOOST_TEST(*second.revision == 2u);
    BOOST_TEST(second.operation_id != first.operation_id);
    struct stat info{};
    BOOST_REQUIRE(::stat((scratch.root / "state.sqlite3").c_str(), &info) == 0);
    BOOST_TEST((info.st_mode & 0777) == 0600);
    const auto archive = store->reserve_archive({"attempt"});
    BOOST_TEST(archive.restorable());
    BOOST_TEST(archive.payload.empty());
    BOOST_TEST(store->load_current()->operation_id == second.operation_id);
}

BOOST_AUTO_TEST_CASE(busy_corruption_and_foreign_identity_never_fallback) {
    Scratch scratch;
    auto store = persistence::make_sqlite_store("session-1", scratch.root);
    auto original = state();
    auto first = store->checkpoint(original, persistence::Boundary::BeforeTools);
    {
        sqlite::Database lock((scratch.root / "state.sqlite3").string());
        lock.execute("BEGIN IMMEDIATE");
        BOOST_CHECK_THROW(store->checkpoint(original, persistence::Boundary::RunFinished), persistence::StoreError);
        BOOST_TEST(store->load_current()->operation_id == first.operation_id);
        lock.execute("ROLLBACK");
    }
    auto foreign = persistence::make_sqlite_store("session-2", scratch.root);
    BOOST_CHECK_THROW(foreign->load_current(), persistence::StoreError);
    BOOST_CHECK_THROW(foreign->checkpoint(original, persistence::Boundary::RunFinished), persistence::StoreError);
    {
        sqlite::Database db((scratch.root / "state.sqlite3").string());
        db.execute("UPDATE store_meta SET store_schema_version = 999");
    }
    BOOST_CHECK_THROW(store->load_current(), persistence::StoreError);
    BOOST_CHECK_THROW(store->checkpoint(original, persistence::Boundary::RunFinished), persistence::StoreError);
    std::filesystem::remove(scratch.root / "state.sqlite3");
    std::ofstream(scratch.root / "state.sqlite3") << "corrupt";
    BOOST_CHECK_THROW(store->load_current(), persistence::StoreError);
    BOOST_CHECK_THROW(store->checkpoint(original, persistence::Boundary::RunFinished), persistence::StoreError);
}

BOOST_AUTO_TEST_CASE(killed_transaction_preserves_complete_checkpoint) {
    Scratch scratch;
    auto store = persistence::make_sqlite_store("session-1", scratch.root);
    auto original = state();
    auto receipt = store->checkpoint(original, persistence::Boundary::ResultsReady);
    const auto child = ::fork();
    BOOST_REQUIRE(child >= 0);
    if (child == 0) {
        sqlite::Database db((scratch.root / "state.sqlite3").string());
        db.execute("BEGIN IMMEDIATE; DELETE FROM snapshots");
        ::_exit(0); // No connection/transaction destructor: crash before COMMIT.
    }
    int status = 0;
    BOOST_REQUIRE(::waitpid(child, &status, 0) == child);
    BOOST_REQUIRE(WIFEXITED(status));
    auto restored = store->load_current();
    BOOST_TEST(restored->operation_id == receipt.operation_id);
    BOOST_TEST(nlohmann::json(restored->state) == nlohmann::json(original));
    const auto committed_child = ::fork();
    BOOST_REQUIRE(committed_child >= 0);
    if (committed_child == 0) {
        original.extras = nlohmann::json{{"committed", true}};
        auto writer = persistence::make_sqlite_store("session-1", scratch.root);
        writer->checkpoint(original, persistence::Boundary::RunFinished);
        ::_exit(0); // Committed but no application event/normal shutdown.
    }
    BOOST_REQUIRE(::waitpid(committed_child, &status, 0) == committed_child);
    BOOST_REQUIRE(WIFEXITED(status));
    BOOST_REQUIRE(WEXITSTATUS(status) == 0);
    auto committed = store->load_current();
    BOOST_TEST(*committed->revision == 2u);
    BOOST_TEST(committed->state.extras->at("committed") == true);
}

BOOST_AUTO_TEST_CASE(selecting_sqlite_does_not_silently_replace_json_history) {
    Scratch scratch;
    std::ofstream(scratch.root / "state.json") << "existing state";
    auto store = persistence::make_sqlite_store("session-1", scratch.root);
    BOOST_CHECK_THROW(store->load_current(), persistence::StoreError);
    BOOST_CHECK_THROW(store->checkpoint(state(), persistence::Boundary::RunFinished), persistence::StoreError);
    BOOST_TEST(!std::filesystem::exists(scratch.root / "state.sqlite3"));
}

BOOST_AUTO_TEST_CASE(existing_store_file_permissions_are_narrowed) {
    Scratch scratch;
    auto store = persistence::make_sqlite_store("session-1", scratch.root);
    store->checkpoint(state(), persistence::Boundary::RunFinished);
    const auto path = scratch.root / "state.sqlite3";
    BOOST_REQUIRE(::chmod(path.c_str(), 0644) == 0);
    store->checkpoint(state(), persistence::Boundary::RunFinished);
    struct stat info{};
    BOOST_REQUIRE(::stat(path.c_str(), &info) == 0);
    BOOST_TEST((info.st_mode & 0777) == 0600);
}

BOOST_AUTO_TEST_CASE(foreign_database_is_rejected_before_wal) {
    Scratch scratch;
    const auto directory = scratch.root / "foreign";
    std::filesystem::create_directories(directory);
    const auto path = directory / "state.sqlite3";
    {
        sqlite::Database database(path.string());
        database.execute("CREATE TABLE unrelated(id INTEGER PRIMARY KEY)");
    }
    const auto before = std::filesystem::file_size(path);
    auto store = persistence::make_sqlite_store("session-1", directory);
    BOOST_CHECK_THROW(store->checkpoint(state(), persistence::Boundary::RunFinished), persistence::StoreError);
    BOOST_TEST(!std::filesystem::exists(directory / "state.sqlite3-wal"));
    BOOST_TEST(!std::filesystem::exists(directory / "state.sqlite3-shm"));
    BOOST_TEST(std::filesystem::file_size(path) == before);
}

BOOST_AUTO_TEST_CASE(released_reservation_cannot_start_an_archive) {
    Scratch scratch;
    auto store = persistence::make_sqlite_store("session-1", scratch.root);
    store->checkpoint(state(), persistence::Boundary::RunFinished);
    const auto archive = store->reserve_archive({"rejected"});
    store->release_archive(archive);
    BOOST_CHECK_THROW(store->begin_archive(archive, state()), std::invalid_argument);
}
