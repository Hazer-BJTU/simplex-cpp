#pragma once

#include <string_view>
#include "sqlite/database.hpp"

namespace persistence::sqlite_archive_index {

/** Schema-v2 additions. The source view projects searchable text from immutable
 * snapshot rows; chunks and their external-content FTS index are derived data.
 * Execute only inside the schema owner's write transaction. */
const char* schema_sql();

/** Insert every searchable source of one archive, using bounded UTF-8 chunks.
 * The caller owns the transaction containing the archive publication. */
void index_archive(sqlite::Database& database, std::string_view archive_id);

/** Backfill all existing archives during the v1 -> v2 migration. Processes one
 * source at a time; never reconstructs an entire AgentInputState in memory. */
void backfill(sqlite::Database& database);

/** Require derived tables, source view and synchronization triggers. Missing
 * structures are corruption, not an empty search result. Never repairs. */
void inspect(sqlite::Database& database);

} // namespace persistence::sqlite_archive_index
