#pragma once

#include "persistence/store.hpp"
#include "sqlite/database.hpp"
#include <filesystem>
#include <unistd.h>

namespace archive_test {

/** A minimal real state and private database, shared by index/query tests. */
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("simplex_archive_search_" + std::to_string(getpid()));
    std::unique_ptr<persistence::StateStore> store;

    Fixture() {
        std::filesystem::remove_all(root);
        store = persistence::make_sqlite_store("test", root);
    }
    ~Fixture() {
        store.reset();
        std::filesystem::remove_all(root);
    }
    std::string path() const { return (root / "state.sqlite3").string(); }

    static model_io::AgentInputState state(std::string text) {
        model_io::AgentInputState result;
        result.meta.session_id = "test";
        model_io::UserLoopStep turn;
        turn.user_input.type = model_io::MessageItemType::UserInput;
        turn.user_input.role = "user";
        model_io::Content content;
        content.raw = std::move(text);
        turn.user_input.content.push_back(std::move(content));
        result.turns.push_back(std::move(turn));
        return result;
    }

    persistence::ArchiveRef archive(const model_io::AgentInputState& value) {
        const auto ref = store->reserve_archive({"run"});
        store->begin_archive(ref, value);
        store->finish_archive_attempt(ref, persistence::ArchiveOutcome::Failed);
        return ref;
    }
};

inline std::int64_t scalar(sqlite::Database& database, std::string_view sql) {
    auto result = database.prepare(sql);
    if (result.step() != sqlite::StepResult::Row) throw std::runtime_error("missing test row");
    return result.column_int64(0);
}

/** Recreate the pre-feature schema without changing authoritative rows. */
inline void downgrade_to_v1(sqlite::Database& database) {
    database.execute(
        "DROP TRIGGER archive_chunks_insert; DROP TRIGGER archive_chunks_delete; "
        "DROP TRIGGER archive_chunks_update; DROP TABLE archive_chunks_fts; "
        "DROP TABLE archive_chunks; DROP VIEW archive_text_sources; "
        "UPDATE store_meta SET store_schema_version = 1; "
        "DELETE FROM schema_migrations; "
        "INSERT INTO schema_migrations VALUES(1, 'original');");
}

} // namespace archive_test
