#define BOOST_TEST_MODULE PersistenceSqliteArchive
#include <boost/test/unit_test.hpp>

#include "persistence/store.hpp"
#include "sqlite_snapshot.hpp"
#include "sqlite/database.hpp"

#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>

namespace {
using namespace persistence;
using Json = nlohmann::json;

struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("simplex_sqlite_archive_" + std::to_string(getpid()));
    Scratch() { std::filesystem::remove_all(root); }
    ~Scratch() { std::filesystem::remove_all(root); }
};

model_io::AgentInputState state(const std::string& marker) {
    model_io::AgentInputState value;
    value.meta.session_id = "test";
    value.system_prompt.add_section("system", "System", marker);
    value.extras = Json{{"payload", std::string(1024 * 1024, 'x')}, {"marker", marker},
        {"optional", nullptr}, {"binary", std::string("a\0b", 3)}};
    model_io::UserLoopStep turn;
    turn.user_input.role = "user";
    turn.user_input.type = model_io::MessageItemType::UserInput;
    model_io::Content content;
    content.raw = marker;
    turn.user_input.content.push_back(content);
    model_io::AgentLoopStep step;
    step.model_response.role = "assistant";
    step.model_response.type = model_io::MessageItemType::ModelResponse;
    step.model_response.content.push_back(content);
    step.model_response.cost = model_io::TokenCost{UINT64_MAX, 0, 0};
    turn.agent_loop_step.push_back(step);
    value.turns.push_back(turn);
    value.loop.emplace();
    model_io::InvokeReturn pending;
    pending.query.id = "same-wire-id";
    pending.query.name = "fixture";
    pending.output.raw = std::string(1024 * 1024, 'r');
    value.loop->pending_results.push_back(pending);
    value.loop->phase = model_io::LoopPhase::Projection;
    value.loop->committed_response_sequence = UINT64_MAX;
    return value;
}

std::int64_t scalar(const std::filesystem::path& root, const std::string& sql) {
    sqlite::Database database((root / "state.sqlite3").string(), sqlite::OpenMode::ReadOnly);
    auto query = database.prepare(sql);
    BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
    return query.column_int64(0);
}

model_io::AgentInputState archived(const std::filesystem::path& root, const ArchiveRef& ref) {
    sqlite::Database database((root / "state.sqlite3").string(), sqlite::OpenMode::ReadOnly);
    auto query = database.prepare("SELECT original_snapshot_id FROM archives WHERE archive_id = ?1");
    query.bind_text(1, ref.archive_id);
    BOOST_REQUIRE(query.step() == sqlite::StepResult::Row);
    return sqlite_snapshot::decode_snapshot(database, query.column_int64(0));
}

ArchiveRef prepare(StateStore& store, const model_io::AgentInputState& original) {
    auto ref = store.reserve_archive({"run", true, "auto-compact-call"});
    store.begin_archive(ref, original);
    return ref;
}
}

BOOST_AUTO_TEST_CASE(original_is_frozen_and_success_is_atomic) {
    Scratch scratch;
    auto store = make_sqlite_store("test", scratch.root);
    auto previous = state("last checkpoint");
    const auto saved = store->checkpoint(previous, Boundary::RunFinished);
    const auto original = state("new unsaved edits");
    const auto ref = prepare(*store, original);
    BOOST_TEST(ref.payload.empty());
    BOOST_TEST(ref.restorable());
    BOOST_TEST(Json(archived(scratch.root, ref)) == Json(original));
    BOOST_TEST(Json(store->load_current()->state) == Json(original));
    const auto prepared_revision = store->load_current()->revision;
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives WHERE mode='automatic' AND call_id='auto-compact-call' AND outcome='prepared'") == 1);
    // A separate writer can transact while the model would be suspended.
    sqlite::Database reader((scratch.root / "state.sqlite3").string());
    { sqlite::Transaction transaction(reader); reader.execute("UPDATE sessions SET updated_at=updated_at"); transaction.commit(); }
    const auto replacement = state("summary");
    const auto receipt = store->commit_compaction(ref, replacement, prepared_revision);
    BOOST_TEST(*receipt.revision == *saved.revision + 2);
    BOOST_TEST(store->load_current()->operation_id == receipt.operation_id);
    BOOST_TEST(Json(store->load_current()->state) == Json(replacement));
    BOOST_TEST(Json(archived(scratch.root, ref)) == Json(original));
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives WHERE outcome='succeeded' AND replacement_revision=3") == 1);
    store->finish_archive_attempt(ref, ArchiveOutcome::Failed);
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives WHERE outcome='succeeded'") == 1);
    BOOST_CHECK_THROW(store->commit_compaction(ref, previous), StoreError);
}

BOOST_AUTO_TEST_CASE(restarted_writer_can_commit_an_adopted_attempt) {
    Scratch scratch;
    ArchiveRef ref;
    {
        auto store = make_sqlite_store("test", scratch.root);
        store->checkpoint(state("checkpoint"), Boundary::RunFinished);
        ref = prepare(*store, state("original"));
    }
    auto resumed = make_sqlite_store("test", scratch.root);
    const auto base = resumed->load_current()->revision;
    const auto replacement = state("summary");
    BOOST_CHECK_NO_THROW(resumed->commit_compaction(ref, replacement, base));
    BOOST_TEST(Json(resumed->load_current()->state) == Json(replacement));
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives WHERE outcome='succeeded'") == 1);
}

BOOST_AUTO_TEST_CASE(restarted_writer_can_settle_an_adopted_attempt) {
    Scratch scratch;
    ArchiveRef ref;
    {
        auto store = make_sqlite_store("test", scratch.root);
        store->checkpoint(state("checkpoint"), Boundary::RunFinished);
        ref = prepare(*store, state("original"));
    }
    auto resumed = make_sqlite_store("test", scratch.root);
    BOOST_CHECK_NO_THROW(resumed->finish_archive_attempt(ref, ArchiveOutcome::Cancelled));
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives WHERE outcome='cancelled'") == 1);
}

BOOST_AUTO_TEST_CASE(stale_or_foreign_attempt_does_not_overwrite_current) {
    Scratch scratch;
    auto store = make_sqlite_store("test", scratch.root);
    auto original = state("before");
    store->checkpoint(original, Boundary::RunFinished);
    const auto stale = store->reserve_archive({"stale-run"});
    BOOST_CHECK_THROW(store->begin_archive(stale, original, 0), StoreError);
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives") == 0);
    const auto ref = prepare(*store, original);
    auto after = state("later checkpoint");
    store->checkpoint(after, Boundary::RunFinished);
    BOOST_CHECK_THROW(store->commit_compaction(ref, original), StoreError);
    BOOST_TEST(Json(store->load_current()->state) == Json(after));
    auto foreign = ref;
    foreign.session_id = "other";
    BOOST_CHECK_THROW(store->finish_archive_attempt(foreign, ArchiveOutcome::Failed), std::invalid_argument);
    store->finish_archive_attempt(ref, ArchiveOutcome::Cancelled);
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives WHERE outcome='cancelled'") == 1);
}

BOOST_AUTO_TEST_CASE(retention_preserves_current_and_removes_only_owned_graphs) {
    for (std::size_t maximum : {0u, 1u, 5u}) {
        Scratch scratch;
        auto store = make_sqlite_store("test", scratch.root);
        const auto original = state("current");
        store->checkpoint(original, Boundary::RunFinished);
        std::vector<ArchiveRef> refs;
        for (int index = 0; index < 7; ++index) {
            refs.push_back(prepare(*store, original));
            store->finish_archive_attempt(refs.back(), index % 2 ? ArchiveOutcome::Failed : ArchiveOutcome::Cancelled);
        }
        std::ofstream(scratch.root / "state.md") << "legacy evidence";
        ArchivePruneRequest request{refs.back(), {}, {maximum}};
        const auto removed = store->prune_archives(request);
        const auto retained = maximum == 0 ? 7 : maximum;
        BOOST_TEST(removed.removed_archives == 7 - retained);
        BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives") == retained);
        BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM snapshots WHERE kind='archive'") == retained);
        BOOST_TEST(Json(store->load_current()->state) == Json(original));
        BOOST_TEST(std::filesystem::exists(scratch.root / "state.md"));
        BOOST_TEST(Json(archived(scratch.root, refs.back())) == Json(original));
    }
}

BOOST_AUTO_TEST_CASE(crash_boundaries_preserve_evidence_without_replaying_summary) {
    for (int boundary : {0, 1, 2, 3}) {
        Scratch scratch;
        auto store = make_sqlite_store("test", scratch.root);
        const auto original = state("original");
        store->checkpoint(original, Boundary::RunFinished);
        store.reset();
        const auto child = fork();
        BOOST_REQUIRE(child >= 0);
        if (child == 0) {
            try {
                auto child_store = make_sqlite_store("test", scratch.root);
                if (boundary == 0) {
                    sqlite::Database database((scratch.root / "state.sqlite3").string());
                    database.execute("PRAGMA foreign_keys=ON");
                    sqlite::Transaction transaction(database);
                    sqlite_snapshot::store_archive_snapshot(database, "test", original, false);
                    _exit(0); // T1 graph exists only in an uncommitted transaction.
                }
                auto ref = prepare(*child_store, original);
                if (boundary == 3) {
                    sqlite::Database database((scratch.root / "state.sqlite3").string());
                    database.execute("PRAGMA foreign_keys=ON");
                    sqlite::Transaction transaction(database);
                    sqlite_snapshot::replace_current(database, "test", state("summary"), false);
                    database.execute("UPDATE archives SET outcome='succeeded'");
                    _exit(0); // T2 changed graph and metadata, but neither committed.
                }
                if (boundary == 2) child_store->commit_compaction(ref, state("summary"));
                _exit(0); // T1 or T2 committed, no application event delivered.
            } catch (...) { _exit(1); }
        }
        int status = 0;
        BOOST_REQUIRE(waitpid(child, &status, 0) == child);
        BOOST_REQUIRE(WIFEXITED(status));
        BOOST_REQUIRE(WEXITSTATUS(status) == 0);
        store = make_sqlite_store("test", scratch.root);
        const auto recovered = store->load_current();
        BOOST_REQUIRE(recovered);
        BOOST_TEST(Json(recovered->state) == Json(boundary == 2 ? state("summary") : original));
        BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM snapshots WHERE kind='archive'") == (boundary == 0 ? 0 : 1));
        // Read-only restore does not rewrite attempts; the next writer reconciles.
        store->checkpoint(recovered->state, Boundary::RunFinished, recovered->revision);
        if (boundary == 1 || boundary == 3) {
            BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives WHERE outcome='interrupted'") == 1);
            auto latest = prepare(*store, original);
            store->finish_archive_attempt(latest, ArchiveOutcome::Failed);
            store->prune_archives({latest, {}, {1}});
            BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives") == 1);
        }
        if (boundary == 2) BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives WHERE outcome='succeeded'") == 1);
    }
}

BOOST_AUTO_TEST_CASE(repeated_t1_crashes_do_not_accumulate_resolved_archives) {
    Scratch scratch;
    const auto original = state("original");
    auto store = make_sqlite_store("test", scratch.root);
    store->checkpoint(original, Boundary::RunFinished);
    for (int cycle = 0; cycle < 3; ++cycle) {
        store.reset();
        const auto child = fork();
        BOOST_REQUIRE(child >= 0);
        if (child == 0) {
            try {
                auto writer = make_sqlite_store("test", scratch.root);
                prepare(*writer, original);
                _exit(0);
            } catch (...) { _exit(1); }
        }
        int status = 0;
        BOOST_REQUIRE(waitpid(child, &status, 0) == child);
        BOOST_REQUIRE(WIFEXITED(status));
        BOOST_REQUIRE(WEXITSTATUS(status) == 0);
        store = make_sqlite_store("test", scratch.root);
        const auto recovered = store->load_current();
        BOOST_REQUIRE(recovered);
        BOOST_TEST(Json(recovered->state) == Json(original));
        // Recovery establishes a new authoritative revision before retention.
        store->checkpoint(recovered->state, Boundary::RunFinished, recovered->revision);
        const auto current = prepare(*store, original);
        store->commit_compaction(current, original);
        store->prune_archives({current, {}, {1}});
        BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives") == 1);
        BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM snapshots WHERE kind='archive'") == 1);
        BOOST_TEST(Json(archived(scratch.root, current)) == Json(original));
    }
}

BOOST_AUTO_TEST_CASE(unresolved_interruption_is_protected_until_authoritative_state_advances) {
    Scratch scratch;
    auto store = make_sqlite_store("test", scratch.root);
    const auto original = state("original");
    const auto older = prepare(*store, original);
    store->finish_archive_attempt(older, ArchiveOutcome::Failed);
    const auto interrupted = prepare(*store, original);
    store.reset(); // Restart before settlement, without a later checkpoint.
    store = make_sqlite_store("test", scratch.root);
    const auto recovered = store->load_current();
    BOOST_REQUIRE(recovered);
    // The first mutation reconciles prepared -> interrupted, but merely loading
    // the old revision is insufficient to discard its original recovery graph.
    store->prune_archives({older, {}, {1}});
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives") == 2);
    BOOST_TEST(Json(archived(scratch.root, interrupted)) == Json(original));
    store->checkpoint(recovered->state, Boundary::RunFinished, recovered->revision);
    store->prune_archives({older, {}, {1}});
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives") == 1);
    BOOST_TEST(Json(archived(scratch.root, older)) == Json(original));
}

BOOST_AUTO_TEST_CASE(archive_can_precede_first_checkpoint) {
    Scratch scratch;
    auto store = make_sqlite_store("test", scratch.root);
    const auto original = state("unsaved original");
    const auto ref = prepare(*store, original);
    BOOST_TEST(Json(store->load_current()->state) == Json(original));
    BOOST_TEST(Json(archived(scratch.root, ref)) == Json(original));
    const auto receipt = store->commit_compaction(ref, state("summary"), 1);
    BOOST_TEST(*receipt.revision == 2u);
    BOOST_TEST(Json(store->load_current()->state) == Json(state("summary")));
}

BOOST_AUTO_TEST_CASE(retention_failure_rolls_back_without_changing_committed_replacement) {
    Scratch scratch;
    auto store = make_sqlite_store("test", scratch.root);
    const auto original = state("original");
    store->checkpoint(original, Boundary::RunFinished);
    auto old = prepare(*store, original);
    store->finish_archive_attempt(old, ArchiveOutcome::Failed);
    auto current = prepare(*store, original);
    const auto receipt = store->commit_compaction(current, state("summary"));
    {
        sqlite::Database database((scratch.root / "state.sqlite3").string());
        database.execute("CREATE TRIGGER test_refuse_cleanup BEFORE DELETE ON archives "
            "BEGIN SELECT RAISE(ABORT, 'injected cleanup failure'); END");
    }
    BOOST_CHECK_THROW(store->prune_archives({current, {}, {1}}), StoreError);
    BOOST_TEST(store->load_current()->operation_id == receipt.operation_id);
    BOOST_TEST(Json(store->load_current()->state) == Json(state("summary")));
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives") == 2);
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM snapshots WHERE kind='archive'") == 2);
}

BOOST_AUTO_TEST_CASE(busy_preparation_does_not_publish_an_archive_or_overwrite_current) {
    Scratch scratch;
    auto store = make_sqlite_store("test", scratch.root);
    const auto original = state("original");
    const auto receipt = store->checkpoint(original, Boundary::RunFinished);
    auto ref = store->reserve_archive({"run"});
    {
        sqlite::Database database((scratch.root / "state.sqlite3").string());
        sqlite::Transaction transaction(database);
        database.execute("UPDATE sessions SET updated_at=updated_at");
        BOOST_CHECK_THROW(store->begin_archive(ref, state("unsaved"), receipt.revision), StoreError);
    }
    BOOST_TEST(Json(store->load_current()->state) == Json(original));
    BOOST_TEST(store->load_current()->operation_id == receipt.operation_id);
    BOOST_TEST(scalar(scratch.root, "SELECT COUNT(*) FROM archives") == 0);
    ref = store->reserve_archive({"retry-with-new-identity"});
    BOOST_CHECK_NO_THROW(store->begin_archive(ref, state("unsaved"), receipt.revision));
}
