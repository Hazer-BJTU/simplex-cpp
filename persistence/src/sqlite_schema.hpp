#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite/database.hpp"

/**
 * Executable SQLite schema v1 for one session's authoritative store.
 *
 * The entity mapping is documented in persistence/README.md. This module owns
 * table creation, version checks and transactional migrations; the persistence::StateStore adapter uses this module to validate schema
 * and identity before checkpointing.
 *
 * A store is one database file with one sessions row. Snapshot graphs are
 * normalized: every child row carries snapshot_id and composite foreign keys
 * make cross-snapshot references impossible. `store_meta` is authoritative for
 * the schema/codec version; PRAGMA application_id identifies the file kind so a
 * foreign database is refused instead of being migrated.
 */
namespace persistence::sqlite_schema {

/** Table/index layout version understood by this build. */
inline constexpr std::int64_t schema_version = 1;
/** Row/codec encoding version understood by this build. */
inline constexpr std::int64_t codec_version = 1;
/** PRAGMA application_id for a Simplex session store ("SPLX"). */
inline constexpr std::int64_t application_id = 0x53504C58;

/** Why a database cannot be used as this session's store. */
enum class Reason {
    /** application_id is not a Simplex store. */
    ForeignDatabase,
    /** The store was written by a newer schema; never modify it. */
    NewerSchema,
    /** The store needs a migration that a read-only inspection may not run. */
    UpgradeRequired,
    /** The store uses a row encoding this build cannot decode. */
    UnsupportedCodec,
    /** A claimed Simplex store is missing required metadata or session data. */
    IncompleteSchema,
    /** The database belongs to another session. */
    SessionMismatch,
};

class SchemaError : public std::runtime_error {
public:
    SchemaError(Reason reason, const std::string& message);
    ~SchemaError() override;

    Reason reason() const noexcept { return reason_; }

private:
    Reason reason_;
};

/** Identity read from `store_meta` plus the owning session. */
struct StoreIdentity {
    std::string store_id;
    std::string session_id;
    std::int64_t schema_version = 0;
    std::int64_t codec_version = 0;
};

/**
 * Open, create or upgrade the store for one session.
 *
 * Requires a read/write connection. A new database file gets schema v1,
 * store metadata and its session row in one transaction. An existing store is
 * checked for the expected application id, a supported schema/codec version and
 * a matching session before anything is written. A newer schema is refused
 * without modification. Connection pragmas required for correctness
 * (`foreign_keys=ON`, WAL, `synchronous=FULL`) are applied here.
 */
StoreIdentity open_store(
    sqlite::Database& database, std::string_view session_id,
    std::string_view created_at);

/**
 * Verify an existing store without writing anything.
 *
 * Intended for a read-only connection: it never creates, migrates or repairs.
 * A missing file fails at open; a foreign database, a newer schema, a schema
 * that would need an upgrade, or a different session is an explicit
 * SchemaError. `store_meta` must exist and be readable.
 */
StoreIdentity inspect_store(
    sqlite::Database& database, std::string_view session_id);

/** One ordered, transactional schema upgrade. */
struct Migration {
    std::int64_t version;
    const char* sql;
};

/**
 * Apply every migration newer than `from_version` in one transaction.
 *
 * DDL and the `schema_migrations` row commit together, so a failing migration
 * leaves both the schema and the recorded version unchanged. Exposed for the
 * rollback test; production callers go through open_store().
 */
void apply_migrations(
    sqlite::Database& database, std::int64_t from_version,
    const std::vector<Migration>& migrations, std::string_view applied_at);

} // namespace persistence::sqlite_schema
