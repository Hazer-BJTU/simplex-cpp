#include "sqlite_schema.hpp"

#include "persistence/store.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace persistence::sqlite_schema {
namespace {

/**
 * Complete v1 layout.
 *
 * Version 1 is the baseline: it creates every table and index. Comments explain
 * the encodings the codec implements, not storage policy. A later migration
 * records itself in schema_migrations and updates store_meta in the same SQL, so
 * apply_migrations() cannot leave a half-applied version behind.
 */
constexpr const char* kSchemaV1 = R"sql(
CREATE TABLE store_meta (
  id                   INTEGER PRIMARY KEY CHECK (id = 1),
  store_schema_version INTEGER NOT NULL,
  codec_version        INTEGER NOT NULL,
  store_id             TEXT    NOT NULL,
  created_at           TEXT    NOT NULL
) STRICT;

CREATE TABLE schema_migrations (
  version    INTEGER PRIMARY KEY,
  applied_at TEXT NOT NULL
) STRICT;

-- The current snapshot is derived from the partial unique index on snapshots;
-- no cyclic foreign key from sessions.
CREATE TABLE sessions (
  session_id TEXT PRIMARY KEY,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
) STRICT;

CREATE TABLE snapshots (
  snapshot_id          INTEGER PRIMARY KEY,
  session_id           TEXT    NOT NULL REFERENCES sessions(session_id),
  kind                 TEXT    NOT NULL CHECK (kind IN ('current','archive')),
  revision             INTEGER NOT NULL CHECK (revision >= 0),
  operation_id         TEXT    NOT NULL,
  meta_schema_version  INTEGER NOT NULL,
  session_status       TEXT    NOT NULL CHECK (session_status IN ('active','completed','failed')),
  meta_error_json      TEXT,
  created_at           TEXT    NOT NULL,   -- meta.created_at
  updated_at           TEXT    NOT NULL,   -- meta.updated_at
  heading_level        INTEGER NOT NULL,   -- Not range-checked: the dataclass accepts any int
  extras_json          TEXT                -- NULL = absent; 'null' = present null; else raw JSON
) STRICT;

CREATE UNIQUE INDEX snapshots_one_current ON snapshots(session_id) WHERE kind = 'current';

CREATE TABLE prompt_sections (
  snapshot_id INTEGER NOT NULL REFERENCES snapshots(snapshot_id) ON DELETE CASCADE,
  ordinal     INTEGER NOT NULL CHECK (ordinal >= 0),
  name        TEXT    NOT NULL,
  title       TEXT    NOT NULL,
  stability   TEXT    NOT NULL CHECK (stability IN ('immutable','growing','volatile')),
  body        TEXT    NOT NULL,
  PRIMARY KEY (snapshot_id, ordinal),
  UNIQUE (snapshot_id, name)
) STRICT;

CREATE TABLE tool_definitions (
  snapshot_id          INTEGER NOT NULL REFERENCES snapshots(snapshot_id) ON DELETE CASCADE,
  ordinal              INTEGER NOT NULL CHECK (ordinal >= 0),
  name                 TEXT    NOT NULL,
  description          TEXT    NOT NULL,
  argument_schema_json TEXT    NOT NULL,
  remote_type          TEXT,              -- absent = NULL
  extras_json          TEXT,              -- absent = NULL; present null stores the text 'null'
  PRIMARY KEY (snapshot_id, ordinal)
) STRICT;

CREATE TABLE turns (
  turn_id         INTEGER PRIMARY KEY,
  snapshot_id     INTEGER NOT NULL REFERENCES snapshots(snapshot_id) ON DELETE CASCADE,
  ordinal         INTEGER NOT NULL CHECK (ordinal >= 0),
  retain_priority TEXT    NOT NULL CHECK (retain_priority IN ('normal','discardable','pinned')),
  extras_json     TEXT,
  UNIQUE (snapshot_id, ordinal),
  UNIQUE (snapshot_id, turn_id)          -- Lets child tables use a composite foreign key
) STRICT;

CREATE TABLE agent_steps (
  step_id                INTEGER PRIMARY KEY,
  snapshot_id            INTEGER NOT NULL,
  turn_id                INTEGER NOT NULL,
  ordinal                INTEGER NOT NULL CHECK (ordinal >= 0),
  retain_priority        TEXT    NOT NULL CHECK (retain_priority IN ('normal','discardable','pinned')),
  commit_sequence_u64    TEXT    NOT NULL CHECK (length(commit_sequence_u64) = 20 AND commit_sequence_u64 NOT GLOB '*[^0-9]*'),
  invoke_returns_present INTEGER NOT NULL CHECK (invoke_returns_present IN (0,1)),
  extras_json            TEXT,
  UNIQUE (snapshot_id, step_id),
  UNIQUE (turn_id, ordinal),
  FOREIGN KEY (snapshot_id, turn_id) REFERENCES turns(snapshot_id, turn_id) ON DELETE CASCADE
) STRICT;

CREATE TABLE messages (
  message_id          INTEGER PRIMARY KEY,
  snapshot_id         INTEGER NOT NULL REFERENCES snapshots(snapshot_id) ON DELETE CASCADE,
  turn_id             INTEGER,   -- only the turn user input
  step_id             INTEGER,   -- only model responses and invoke results
  slot                TEXT    NOT NULL CHECK (slot IN ('user_input','model_response','invoke_return')),
  ordinal             INTEGER NOT NULL CHECK (ordinal >= 0),  -- position inside the step; 0 elsewhere
  type                TEXT    NOT NULL CHECK (type IN ('user_input','model_response','invoke_return')),
  role                TEXT    NOT NULL,
  invokes_present     INTEGER NOT NULL CHECK (invokes_present IN (0,1)),
  extras_json         TEXT,
  CHECK ((slot = 'user_input') = (turn_id IS NOT NULL)),
  CHECK ((slot = 'user_input') = (step_id IS NULL)),
  FOREIGN KEY (snapshot_id, turn_id) REFERENCES turns(snapshot_id, turn_id) ON DELETE CASCADE,
  FOREIGN KEY (snapshot_id, step_id) REFERENCES agent_steps(snapshot_id, step_id) ON DELETE CASCADE
) STRICT;

CREATE UNIQUE INDEX messages_one_user_input ON messages(turn_id) WHERE slot = 'user_input';
CREATE UNIQUE INDEX messages_step_position ON messages(step_id, slot, ordinal) WHERE slot <> 'user_input';

CREATE TABLE message_contents (
  message_id  INTEGER NOT NULL REFERENCES messages(message_id) ON DELETE CASCADE,
  slot        TEXT    NOT NULL CHECK (slot IN ('body','reasoning','action_status')),
  ordinal     INTEGER NOT NULL CHECK (ordinal >= 0),
  type        TEXT    NOT NULL CHECK (type IN ('text','binary','external_ref')),
  modality    TEXT    NOT NULL CHECK (modality IN ('text','image','audio','video','document')),
  raw         TEXT    NOT NULL,            -- verbatim, including base64 and embedded NUL
  extras_json TEXT,
  PRIMARY KEY (message_id, slot, ordinal)
) STRICT;

CREATE UNIQUE INDEX message_contents_single ON message_contents(message_id, slot) WHERE slot <> 'body';

CREATE TABLE message_costs (
  message_id      INTEGER PRIMARY KEY REFERENCES messages(message_id) ON DELETE CASCADE,
  prompt_u64      TEXT NOT NULL CHECK (length(prompt_u64) = 20 AND prompt_u64 NOT GLOB '*[^0-9]*'),
  generated_u64   TEXT NOT NULL CHECK (length(generated_u64) = 20 AND generated_u64 NOT GLOB '*[^0-9]*'),
  cache_hit_u64   TEXT NOT NULL CHECK (length(cache_hit_u64) = 20 AND cache_hit_u64 NOT GLOB '*[^0-9]*')
) STRICT;

CREATE TABLE message_invokes (
  message_id      INTEGER NOT NULL REFERENCES messages(message_id) ON DELETE CASCADE,
  ordinal         INTEGER NOT NULL CHECK (ordinal >= 0),
  query_type      TEXT NOT NULL CHECK (query_type IN ('read_only','parall_write','serial_write')),
  query_security  TEXT NOT NULL CHECK (query_security IN ('default_deny','require_confirm','trusted')),
  wire_call_id    TEXT NOT NULL,           -- InvokeQuery.id: may be empty, may repeat, never unique
  name            TEXT NOT NULL,
  arguments_json  TEXT NOT NULL,
  extras_json     TEXT,
  PRIMARY KEY (message_id, ordinal)
) STRICT;

-- one MessageItem::invoke_return (at most one)
CREATE TABLE message_invoke_returns (
  message_id            INTEGER PRIMARY KEY REFERENCES messages(message_id) ON DELETE CASCADE,
  query_type            TEXT NOT NULL CHECK (query_type IN ('read_only','parall_write','serial_write')),
  query_security        TEXT NOT NULL CHECK (query_security IN ('default_deny','require_confirm','trusted')),
  wire_call_id          TEXT NOT NULL,
  query_name            TEXT NOT NULL,
  query_arguments_json  TEXT NOT NULL,
  query_extras_json     TEXT,
  output_type           TEXT NOT NULL CHECK (output_type IN ('text','binary','external_ref')),
  output_modality       TEXT NOT NULL CHECK (output_modality IN ('text','image','audio','video','document')),
  output_raw            TEXT NOT NULL,
  output_extras_json    TEXT,
  extras_json           TEXT
) STRICT;

CREATE TABLE loop_progress (
  snapshot_id                     INTEGER PRIMARY KEY REFERENCES snapshots(snapshot_id) ON DELETE CASCADE,
  status                          TEXT NOT NULL CHECK (status IN ('idle','running','completed','cancelled','exchange_limit','failed','auto_compact_required')),
  phase                           TEXT NOT NULL CHECK (phase IN ('ready','model','tools','projection','blocked')),
  completed_exchanges_u64         TEXT NOT NULL CHECK (length(completed_exchanges_u64) = 20 AND completed_exchanges_u64 NOT GLOB '*[^0-9]*'),
  committed_response_sequence_u64 TEXT NOT NULL CHECK (length(committed_response_sequence_u64) = 20 AND committed_response_sequence_u64 NOT GLOB '*[^0-9]*'),
  error                           TEXT NOT NULL
) STRICT;

CREATE TABLE pending_results (
  snapshot_id           INTEGER NOT NULL REFERENCES snapshots(snapshot_id) ON DELETE CASCADE,
  ordinal               INTEGER NOT NULL CHECK (ordinal >= 0),
  query_type            TEXT NOT NULL CHECK (query_type IN ('read_only','parall_write','serial_write')),
  query_security        TEXT NOT NULL CHECK (query_security IN ('default_deny','require_confirm','trusted')),
  wire_call_id          TEXT NOT NULL,
  query_name            TEXT NOT NULL,
  query_arguments_json  TEXT NOT NULL,
  query_extras_json     TEXT,
  output_type           TEXT NOT NULL CHECK (output_type IN ('text','binary','external_ref')),
  output_modality       TEXT NOT NULL CHECK (output_modality IN ('text','image','audio','video','document')),
  output_raw            TEXT NOT NULL,
  output_extras_json    TEXT,
  extras_json           TEXT,
  PRIMARY KEY (snapshot_id, ordinal)
) STRICT;

CREATE TABLE archives (
  archive_id           TEXT PRIMARY KEY,   -- stable identity; migrated JSON archives keep their directory name
  session_id           TEXT NOT NULL REFERENCES sessions(session_id),
  archive_kind         TEXT NOT NULL CHECK (archive_kind IN ('snapshot')),  -- memory archives are deferred to a later migration
  sequence             INTEGER NOT NULL CHECK (sequence > 0),
  storage_kind         TEXT NOT NULL CHECK (storage_kind IN ('sqlite','json_state','legacy_readable')),
  legacy_locator       TEXT,               -- pre-migration JSON/Markdown location; NULL for new archives
  original_snapshot_id INTEGER REFERENCES snapshots(snapshot_id),  -- NULL for legacy_readable
  mode                 TEXT NOT NULL CHECK (mode IN ('manual','automatic')),
  run_id               TEXT NOT NULL,
  call_id              TEXT,
  outcome              TEXT NOT NULL CHECK (outcome IN ('prepared','succeeded','failed','cancelled','interrupted')),
  created_at           TEXT NOT NULL,
  replacement_revision INTEGER,
  diagnostic_json      TEXT,
  UNIQUE (session_id, sequence)
) STRICT;
)sql";

/**
 * Optional access-path indexes, not a codec/schema migration. Foreign-key
 * cascades do not include slot predicates, so the partial uniqueness indexes
 * cannot serve them. Without these full owner indexes, replacing one snapshot
 * repeatedly scans messages in every retained archive for each deleted parent.
 * Only writable opens install them; older v1 files remain readable unchanged.
 */
void ensure_access_indexes(sqlite::Database& database) {
    sqlite::Transaction transaction(database);
    database.execute(
        "CREATE INDEX IF NOT EXISTS messages_turn_owner "
        "ON messages(snapshot_id, turn_id);");
    database.execute(
        "CREATE INDEX IF NOT EXISTS messages_step_owner "
        "ON messages(snapshot_id, step_id);");
    transaction.commit();
}

/** Ordered baseline; open_store() applies anything newer than the file's version. */
constexpr Migration kMigrations[] = {{1, kSchemaV1}};

/** Read one integer PRAGMA that returns a single row. */
std::int64_t pragma_int(sqlite::Database& database, std::string_view sql) {
    auto query = database.prepare(sql);
    if (query.step() != sqlite::StepResult::Row) {
        throw std::runtime_error("SQLite PRAGMA returned no value");
    }
    return query.column_int64(0);
}

bool has_table(sqlite::Database& database, std::string_view name) {
    auto query = database.prepare(
        "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = ?1");
    query.bind_text(1, name);
    if (query.step() != sqlite::StepResult::Row) {
        throw std::runtime_error("cannot inspect SQLite schema");
    }
    return query.column_int64(0) != 0;
}

bool has_index(sqlite::Database& database, std::string_view name) {
    auto query = database.prepare(
        "SELECT count(*) FROM sqlite_master WHERE type = 'index' AND name = ?1");
    query.bind_text(1, name);
    if (query.step() != sqlite::StepResult::Row) {
        throw std::runtime_error("cannot inspect SQLite schema");
    }
    return query.column_int64(0) != 0;
}

/** Tables and indexes whose absence silently weakens the codec's invariants. */
void require_layout(sqlite::Database& database) {
    static constexpr std::string_view tables[] = {
        "store_meta",       "schema_migrations",    "sessions",
        "snapshots",        "prompt_sections",      "tool_definitions",
        "turns",            "agent_steps",          "messages",
        "message_contents", "message_costs",        "message_invokes",
        "message_invoke_returns", "loop_progress",   "pending_results",
        "archives",
    };
    for (const auto table : tables) {
        if (!has_table(database, table)) {
            throw SchemaError(
                Reason::IncompleteSchema,
                "session store is missing required table '" + std::string(table)
                    + "'");
        }
    }
    static constexpr std::string_view indexes[] = {
        "snapshots_one_current",
        "messages_one_user_input",
        "messages_step_position",
        "message_contents_single",
    };
    for (const auto index : indexes) {
        if (!has_index(database, index)) {
            throw SchemaError(
                Reason::IncompleteSchema,
                "session store is missing required index '" + std::string(index)
                    + "'");
        }
    }
}

/** True when the file already holds application tables of any kind. */
bool any_user_table(sqlite::Database& database) {
    auto query = database.prepare(
        "SELECT count(*) FROM sqlite_master "
        "WHERE type = 'table' AND name NOT LIKE 'sqlite_%'");
    if (query.step() != sqlite::StepResult::Row) {
        throw std::runtime_error("cannot inspect SQLite schema");
    }
    return query.column_int64(0) != 0;
}

StoreIdentity read_identity(
    sqlite::Database& database, std::string_view expected_session) {
    StoreIdentity identity;
    auto meta = database.prepare(
        "SELECT store_id, store_schema_version, codec_version "
        "FROM store_meta WHERE id = 1");
    if (meta.step() != sqlite::StepResult::Row) {
        throw SchemaError(
            Reason::IncompleteSchema, "session store has no store_meta row");
    }
    identity.store_id = meta.column_text(0);
    identity.schema_version = meta.column_int64(1);
    identity.codec_version = meta.column_int64(2);

    auto sessions = database.prepare("SELECT session_id FROM sessions");
    if (sessions.step() != sqlite::StepResult::Row) {
        throw SchemaError(
            Reason::IncompleteSchema, "session store has no sessions row");
    }
    identity.session_id = sessions.column_text(0);
    if (sessions.step() == sqlite::StepResult::Row) {
        throw SchemaError(
            Reason::IncompleteSchema, "session store has more than one sessions row");
    }
    if (std::string_view(identity.session_id) != expected_session) {
        throw SchemaError(
            Reason::SessionMismatch,
            "database belongs to session '" + identity.session_id + "', not '"
                + std::string(expected_session) + "'");
    }
    return identity;
}

/** Refuse any version this build cannot interpret as-is. */
void check_decodable(const StoreIdentity& identity) {
    if (identity.schema_version > schema_version) {
        throw SchemaError(
            Reason::NewerSchema,
            "session store schema " + std::to_string(identity.schema_version)
                + " is newer than the supported " + std::to_string(schema_version));
    }
    if (identity.codec_version > codec_version) {
        throw SchemaError(
            Reason::UnsupportedCodec,
            "session store codec " + std::to_string(identity.codec_version)
                + " is newer than the supported " + std::to_string(codec_version));
    }
    if (identity.codec_version != codec_version) {
        throw SchemaError(
            Reason::UnsupportedCodec,
            "session store codec " + std::to_string(identity.codec_version)
                + " needs an encoding migration this build does not provide");
    }
}

/** Create schema v1, its metadata and the owning session in one transaction. */
void create_store(
    sqlite::Database& database, std::string_view session_id,
    std::string_view created_at) {
    sqlite::Transaction transaction(database);
    database.execute(kSchemaV1);
    database.execute(
        "PRAGMA application_id = " + std::to_string(application_id));

    auto meta = database.prepare(
        "INSERT INTO store_meta("
        "id, store_schema_version, codec_version, store_id, created_at) "
        "VALUES(1, ?1, ?2, lower(hex(randomblob(16))), ?3)");
    meta.bind_int64(1, schema_version);
    meta.bind_int64(2, codec_version);
    meta.bind_text(3, created_at);
    if (meta.step() != sqlite::StepResult::Done) {
        throw std::runtime_error("cannot create store metadata");
    }

    auto migration = database.prepare(
        "INSERT INTO schema_migrations(version, applied_at) VALUES(?1, ?2)");
    migration.bind_int64(1, schema_version);
    migration.bind_text(2, created_at);
    if (migration.step() != sqlite::StepResult::Done) {
        throw std::runtime_error("cannot record the baseline schema migration");
    }

    auto session = database.prepare(
        "INSERT INTO sessions(session_id, created_at, updated_at) "
        "VALUES(?1, ?2, ?2)");
    session.bind_text(1, session_id);
    session.bind_text(2, created_at);
    if (session.step() != sqlite::StepResult::Done) {
        throw std::runtime_error("cannot create the session row");
    }
    transaction.commit();
}

} // namespace

SchemaError::SchemaError(Reason reason, const std::string& message)
    : std::runtime_error(message), reason_(reason) {}

SchemaError::~SchemaError() = default;

StoreIdentity open_store(
    sqlite::Database& database, std::string_view session_id,
    std::string_view created_at) {
    // Required for cross-snapshot integrity; journal/synchronous follow #187 §6.
    database.execute("PRAGMA foreign_keys = ON");
    database.execute("PRAGMA journal_mode = WAL");
    database.execute("PRAGMA synchronous = FULL");

    const auto file_application_id = pragma_int(database, "PRAGMA application_id");
    if (!has_table(database, "store_meta")) {
        if (file_application_id == application_id) {
            throw SchemaError(
                Reason::IncompleteSchema,
                "database claims to be a Simplex store but has no store_meta table");
        }
        if (file_application_id != 0 || any_user_table(database)) {
            throw SchemaError(
                Reason::ForeignDatabase,
                "database is not a Simplex session store; refusing to adopt it");
        }
        create_store(database, session_id, created_at);
        ensure_access_indexes(database);
        return read_identity(database, session_id);
    }
    if (file_application_id != application_id) {
        throw SchemaError(
            Reason::ForeignDatabase,
            "database application id " + std::to_string(file_application_id)
                + " is not a Simplex session store");
    }

    auto identity = read_identity(database, session_id);
    check_decodable(identity);
    if (identity.schema_version < schema_version) {
        apply_migrations(
            database, identity.schema_version,
            std::vector<Migration>(std::begin(kMigrations), std::end(kMigrations)),
            utc_timestamp());
        identity = read_identity(database, session_id);
        if (identity.schema_version != schema_version) {
            throw SchemaError(
                Reason::IncompleteSchema,
                "schema migration did not reach version "
                    + std::to_string(schema_version));
        }
        check_decodable(identity);
    }
    require_layout(database);
    ensure_access_indexes(database);
    return identity;
}

StoreIdentity inspect_store(
    sqlite::Database& database, std::string_view session_id) {
    // Harmless on a read-only connection and keeps inspection consistent.
    database.execute("PRAGMA foreign_keys = ON");
    const auto file_application_id = pragma_int(database, "PRAGMA application_id");
    if (file_application_id != application_id) {
        throw SchemaError(
            Reason::ForeignDatabase,
            "database application id " + std::to_string(file_application_id)
                + " is not a Simplex session store");
    }
    if (!has_table(database, "store_meta")) {
        throw SchemaError(
            Reason::IncompleteSchema, "session store has no store_meta table");
    }
    const auto identity = read_identity(database, session_id);
    if (identity.schema_version > schema_version) {
        throw SchemaError(
            Reason::NewerSchema,
            "session store schema " + std::to_string(identity.schema_version)
                + " is newer than the supported " + std::to_string(schema_version));
    }
    if (identity.schema_version < schema_version) {
        throw SchemaError(
            Reason::UpgradeRequired,
            "session store schema " + std::to_string(identity.schema_version)
                + " needs an upgrade; inspection never migrates");
    }
    if (identity.codec_version != codec_version) {
        throw SchemaError(
            Reason::UnsupportedCodec,
            "session store codec " + std::to_string(identity.codec_version)
                + " is not supported by this build");
    }
    require_layout(database);
    return identity;
}

void apply_migrations(
    sqlite::Database& database, std::int64_t from_version,
    const std::vector<Migration>& migrations, std::string_view applied_at) {
    sqlite::Transaction transaction(database);
    std::int64_t previous = from_version;
    for (const auto& migration : migrations) {
        if (migration.version <= from_version) {
            previous = std::max(previous, migration.version);
            continue;
        }
        if (migration.version <= previous) {
            throw std::logic_error("schema migrations must be ordered by version");
        }
        database.execute(migration.sql);
        auto record = database.prepare(
            "INSERT INTO schema_migrations(version, applied_at) VALUES(?1, ?2)");
        record.bind_int64(1, migration.version);
        record.bind_text(2, applied_at);
        if (record.step() != sqlite::StepResult::Done) {
            throw std::runtime_error("cannot record a schema migration");
        }
        previous = migration.version;
    }
    transaction.commit();
}

} // namespace persistence::sqlite_schema
