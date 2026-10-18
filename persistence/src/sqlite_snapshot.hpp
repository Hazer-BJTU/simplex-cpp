#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dataclass/model_io.hpp"
#include "sqlite/database.hpp"

/**
 * Row-level codec between AgentInputState and the normalized schema v1.
 *
 * Each snapshot is one complete state graph: the header row plus prompt
 * sections, tool definitions, turns, agent steps, messages, message contents,
 * costs, invokes, invoke returns and loop progress. Child rows carry their
 * snapshot id, and composite foreign keys make a cross-snapshot reference
 * impossible. The codec writes and reads the complete graph in one
 * transaction and never deduplicates content.
 *
 * The schema is owned by sqlite_schema.{hpp,cpp}; this module assumes an
 * already opened store and performs no migration. `replace_current` is the
 * whole-graph rewrite path (D6), while `store_archive_snapshot` only inserts: an
 * archive snapshot is never updated or rewritten. The standalone codec uses
 * placeholder revision/operation metadata; the SQLite StateStore supplies
 * authoritative metadata inside the same transaction.
 *
 * Missing vs empty vs null is preserved per field:
 *   - a missing optional row/column is absent;
 *   - an engaged `optional<json>` holding JSON null is stored as the text
 *     `null`, distinguishable from SQL NULL;
 *   - an empty array keeps its presence flag (`invokes_present`,
 *     `invoke_returns_present`) or stores `[]` inside `extras_json`;
 *   - empty strings stay `TEXT ''`.
 *
 * A damaged graph is reported as SnapshotError instead of being returned as an
 * empty session: missing cardinality (one user input per turn, one model
 * response per step), a missing invoke-return batch, invalid JSON or an
 * unreadable enum name are all corruption.
 */
namespace persistence::sqlite_snapshot {

/** Whether a snapshot is the session's current state or immutable evidence. */
enum class SnapshotKind {
    Current,
    Archive,
};

/** Why a snapshot could not be encoded, decoded or attached to its session. */
enum class Reason {
    /** The state names a different session than the store. */
    SessionMismatch,
    /** The database has no sessions row for the requested session. */
    UnknownSession,
    /** The state cannot be represented (for example an invalid enum value). */
    InvalidState,
    /** No snapshot with the requested row id exists. */
    SnapshotNotFound,
    /** The stored graph violates a codec invariant or contains invalid JSON. */
    CorruptSnapshot,
};

class SnapshotError : public std::runtime_error {
public:
    SnapshotError(Reason reason, const std::string& message);
    ~SnapshotError() override;

    Reason reason() const noexcept { return reason_; }

private:
    Reason reason_;
};

/**
 * Insert one complete snapshot graph in a transaction.
 * With own_transaction=false the caller must commit or roll back its own
 * transaction, allowing attempt metadata and the graph to publish together.
 *
 * Used for the first current snapshot and for tests; `Current` fails if the
 * session already has one (the partial unique index). `replace_current` is the
 * rewrite path for an existing current snapshot, and
 * `store_archive_snapshot` the archive path. The session row must already exist
 * (open_store creates it) and `state.meta.session_id` must equal `session_id`.
 */
std::int64_t encode_snapshot(
    sqlite::Database& database, std::string_view session_id,
    const model_io::AgentInputState& state, SnapshotKind kind, bool own_transaction = true);

/**
 * Insert one immutable archive snapshot graph.
 *
 * Archives are never updated by this module; retention deletes their graphs.
 * With own_transaction=false the caller owns the transaction and must attach
 * the archive metadata before committing; the codec does not publish alone.
 */
std::int64_t store_archive_snapshot(
    sqlite::Database& database, std::string_view session_id,
    const model_io::AgentInputState& state, bool own_transaction = true);

/**
 * Rewrite the session's current snapshot as one complete graph.
 *
 * With own_transaction=false the caller must own the transaction, including
 * commit/rollback and revision metadata. Otherwise runs in one transaction:
 * the existing current snapshot row is deleted (its
 * children cascade) and reinserted under the same snapshot id with the new
 * graph, or a current snapshot is inserted when none exists. `sessions`
 * `updated_at` follows `state.meta.updated_at`. Archive snapshots are not
 * touched. Returns the current snapshot id.
 */
std::int64_t replace_current(
    sqlite::Database& database, std::string_view session_id,
    const model_io::AgentInputState& state, bool own_transaction = true);

/**
 * Load the session's current snapshot.
 *
 * Returns nothing only when the session has no current snapshot. A missing
 * session row, corrupt rows or an inconsistent graph raise SnapshotError rather
 * than looking like a fresh session. Performs no writes and is safe on a
 * read-only connection.
 */
std::optional<model_io::AgentInputState> load_current(
    sqlite::Database& database, std::string_view session_id,
    bool own_transaction = true);

/** Decode one snapshot graph by id in one read transaction. */
model_io::AgentInputState decode_snapshot(
    sqlite::Database& database, std::int64_t snapshot_id);

} // namespace persistence::sqlite_snapshot
