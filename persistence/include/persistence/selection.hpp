#pragma once

#include <filesystem>
#include <string_view>
#include "persistence/store.hpp"

namespace persistence {

/**
 * Select the authoritative backend without creating storage. An empty selection
 * means auto detection: JSON-only stays JSON, SQLite-only stays SQLite, and an
 * empty directory selects SQLite. A directory holding both stores is refused for
 * every setting, because honoring one would abandon the other history.
 * Filesystem errors and incompatible choices propagate; there is no fallback.
 */
Backend select_backend(const std::filesystem::path& directory,
                       std::string_view selection = {});

/**
 * Read a complete SQLite current state in one read-only transaction, including
 * schema validation and revision metadata. The owning session is discovered
 * from the database for target-side management; no database is created or
 * migrated, and no archive-query interface is exposed. Missing storage returns
 * null; a present invalid store throws.
 */
std::optional<StoredState> inspect_sqlite_current(const std::filesystem::path& directory);

} // namespace persistence
