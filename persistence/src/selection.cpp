#include "persistence/selection.hpp"

#include "sqlite_schema.hpp"
#include "sqlite_snapshot.hpp"

namespace persistence {

Backend select_backend(
    const std::filesystem::path& directory, std::string_view selection) {
    if (!selection.empty() && selection != "auto"
        && selection != "json" && selection != "sqlite") {
        throw std::invalid_argument("persistence.backend must be json or sqlite");
    }
    const bool json = std::filesystem::exists(directory / "state.json");
    const bool sqlite = std::filesystem::exists(directory / "state.sqlite3");
    if (json && sqlite) {
        // Two stores cannot be reconciled by a setting: honoring either choice
        // would silently abandon the other history.
        throw std::runtime_error(
            "both JSON and SQLite stores exist; remove or migrate one before "
            "selecting persistence.backend (migration is unavailable)");
    }
    if (selection.empty() || selection == "auto") {
        return json ? Backend::Json : Backend::Sqlite;
    }
    const bool wants_json = selection == "json";
    if ((wants_json && sqlite) || (!wants_json && json)) {
        throw std::runtime_error(
            "backend selection conflicts with existing history; keep its backend "
            "or use an empty directory (migration is unavailable)");
    }
    return wants_json ? Backend::Json : Backend::Sqlite;
}

std::optional<StoredState> inspect_sqlite_current(const std::filesystem::path& directory) {
    const auto path = directory / "state.sqlite3";
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }
    // Same rule as the worker's store: a link is not this session's database,
    // even when its target happens to be readable.
    sqlite_schema::require_regular_store_file(path);
    sqlite::Database database(path.string(), sqlite::OpenMode::ReadOnly);
    sqlite::Transaction transaction(database);
    auto owner = database.prepare("SELECT session_id FROM sessions");
    if (owner.step() != sqlite::StepResult::Row) {
        throw std::runtime_error("SQLite store has no owning session");
    }
    const auto session = owner.column_text(0);
    if (owner.step() == sqlite::StepResult::Row) {
        throw std::runtime_error("SQLite store has multiple owning sessions");
    }
    sqlite_schema::inspect_store(database, session);
    auto state = sqlite_snapshot::load_current(database, session, false);
    if (!state) {
        return std::nullopt;
    }
    auto metadata = database.prepare(
        "SELECT revision, operation_id FROM snapshots WHERE session_id = ?1 AND kind = 'current'");
    metadata.bind_text(1, session);
    if (metadata.step() != sqlite::StepResult::Row) {
        throw std::runtime_error("SQLite current metadata is missing");
    }
    const auto revision = metadata.column_int64(0);
    if (revision < 0) {
        throw std::runtime_error("invalid SQLite revision");
    }
    return StoredState{std::move(*state), Backend::Sqlite,
        static_cast<Revision>(revision), metadata.column_text(1)};
}

} // namespace persistence
