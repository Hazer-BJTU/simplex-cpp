#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "dataclass/model_io.hpp"
#include "load/retention.hpp"

/**
 * Authoritative session persistence, independent of its physical representation.
 *
 * The worker owns one store per session and exactly one writer at a time. A
 * store instance has one serialized owner: the contract has no internal
 * locking, and no two operations may overlap. The worker awaits blocking
 * checkpoints on its dedicated storage executor. Read-only inspection by another
 * process uses the backend's own path; it is not part of this interface.
 *
 * Every operation is synchronous. When a call returns, its durability outcome is
 * known, so a required checkpoint can never be a fire-and-forget write. A backend
 * that needs a database writer or another executor still completes or fails the
 * call before returning; the caller must not resume the loop, dispatch tools or
 * announce a compaction while a write is outstanding. Forcing a checkpoint to
 * wait is the ordering guarantee that makes an ambiguous external tool side
 * effect recoverable, not an implementation detail.
 *
 * This interface is storage-only. It has no memory or archive query surface:
 * archives are reserved, published, settled and pruned as evidence. The separate
 * <persistence/archive_query.hpp> interface provides SQLite archive reads.
 *
 * Codec and filesystem failures are reported as StoreError, so a caller can rely
 * on status() for recovery. Programming errors stay distinguishable: a foreign
 * archive reference, an invalid enum value or an unsupported revision
 * expectation throws a standard exception instead of claiming a storage outcome.
 */
namespace persistence {

/** Authoritative-store identity reported by receipts and archive records. */
enum class Backend {
    /// Atomic `state.json` replacement with per-attempt JSON state archives.
    Json,
    /// Transactional normalized state.sqlite3 store, the new-session default.
    Sqlite,
};

/** Stable wire and diagnostic label for a backend ("json" or "sqlite"). */
const char* backend_name(Backend backend);

/**
 * Required-checkpoint boundaries.
 *
 * The labels are part of the worker protocol: before_tools, results_ready,
 * step_finished, run_finished, cancelled, shutdown and compact.
 */
enum class Boundary {
    BeforeTools,
    ResultsReady,
    StepFinished,
    RunFinished,
    Cancelled,
    Shutdown,
    Compaction,
};

/** Wire label for a boundary; throws for an invalid value. */
const char* boundary_name(Boundary boundary);

/**
 * Outcome of one durable store operation.
 *
 * The distinction matters because a failed publication is not always a failed
 * write: a rename can succeed and the following directory sync fail. A caller
 * must stop further writes on Unknown and verify the store after reopening
 * instead of retrying blindly.
 */
enum class CommitStatus {
    /// The store still holds the previous state; retrying cannot lose data.
    NotCommitted,
    /// The new state is published and synchronized.
    Committed,
    /// Publication may or may not have happened; evidence must be inspected.
    Unknown,
};

/**
 * Store revision used to reject a stale write.
 *
 * Syntax-only identifiers may also be stored in an archive, so a revision is
 * never inferred from a file timestamp. A backend that cannot expose one
 * cheaply reports an empty revision and rejects a non-empty expectation rather
 * than pretending to validate it.
 */
using Revision = std::uint64_t;

/** A decoded current state and the store metadata that identified it. */
struct StoredState {
    model_io::AgentInputState state;
    Backend backend = Backend::Json;
    /// Revision observed by this read; empty when the backend tracks none.
    std::optional<Revision> revision;
    /// Last committed operation, available after reopening uncertain writes.
    std::string operation_id;
};

/** Result of one required checkpoint or compaction commit. */
struct CommitReceipt {
    Backend backend = Backend::Json;
    /// A default-constructed receipt claims nothing, never an accidental success.
    CommitStatus status = CommitStatus::NotCommitted;
    /**
     * Backend-scoped operation identity; empty when the backend cannot verify a
     * commit by identity. It correlates diagnostics and recovery evidence and
     * never makes an external tool effect idempotent.
     */
    std::string operation_id;
    /// Revision after the commit; empty when the backend tracks none.
    std::optional<Revision> revision;
};

/**
 * Failure of a store operation with the outcome the caller can trust.
 *
 * The message keeps the backend's existing diagnostic text, including the file
 * path the JSON codec already reported. status() is the authority for recovery:
 * NotCommitted leaves the previous state intact, while Unknown means the
 * replacement may already be visible with uncertain crash durability.
 */
class StoreError : public std::runtime_error {
public:
    StoreError(
        const std::string& message, Backend backend, CommitStatus status,
        std::string operation_id = {});
    ~StoreError() override;

    Backend backend() const noexcept { return backend_; }
    CommitStatus status() const noexcept { return status_; }
    const std::string& operation_id() const noexcept { return operation_id_; }

private:
    Backend backend_;
    CommitStatus status_;
    std::string operation_id_;
};

/** Physical form of one archived compact attempt. */
enum class ArchiveKind {
    /// Complete JSON state that can be decoded back into an AgentInputState.
    JsonState,
    /// Complete immutable state graph in the session SQLite database.
    SqliteSnapshot,
    /**
     * Write-only Markdown left by a worker that predates complete JSON archives.
     * It cannot be decoded back into state, so it is never restorable and never a
     * retention candidate: an archived Markdown file is preserved as evidence
     * rather than deleted by a later retention pass.
     */
    LegacyReadable,
};

/**
 * Identity of one archived compact attempt.
 *
 * Session and archive identifiers are backend-scoped values, never filesystem
 * paths. The JSON backend reports the complete state payload as `payload` so
 * prompts and the compact result can name the artifact without the worker
 * knowing the layout. The record is storage evidence, not a queryable reference.
 */
struct ArchiveRef {
    std::string session_id;
    /// Stable identity within the session; the JSON archive directory name.
    std::string archive_id;
    Backend backend = Backend::Json;
    /// A default-constructed reference claims the least: a non-restorable artifact.
    ArchiveKind kind = ArchiveKind::LegacyReadable;
    /// Archived payload on disk; empty for identity-only backends.
    std::filesystem::path payload;

    /// Restorability is derived from the kind so the two cannot disagree.
    constexpr bool restorable() const noexcept {
        return kind == ArchiveKind::JsonState || kind == ArchiveKind::SqliteSnapshot;
    }
};

/** Inputs that identify one compact attempt before any state is written. */
struct CompactAttempt {
    /**
     * Execution identity; never reused. The JSON backend requires a UUID because
     * the value forms the archive directory name, and rejects another identity
     * instead of creating a directory that recognition could not verify.
     */
    std::string run_id;
    /// Automatic attempts retain their synthetic tool-call identity as evidence.
    bool automatic = false;
    std::string call_id;
};

/**
 * How an attempt ended without a committed replacement.
 *
 * A committed replacement settles its own attempt inside commit_compaction(), so
 * callers never report success here: success must not be a second, fallible step
 * after the commit.
 */
enum class ArchiveOutcome {
    /// The attempt ended with a failure diagnostic.
    Failed,
    /// The invocation was cancelled before the replacement committed.
    Cancelled,
    /// A backend assigns this when a restart finds an attempt it cannot settle.
    Interrupted,
};

/** Storage reclaimed by one retention pass. */
struct CleanupResult {
    std::size_t removed_archives = 0;
    /// Removed file payload bytes; SQLite reports zero (pages remain reusable).
    std::uintmax_t removed_bytes = 0;
};

/** One settled attempt plus the archives retention must keep. */
struct ArchivePruneRequest {
    /// Attempt that just settled; always retained, counted and required to exist.
    ArchiveRef current;
    /// Archives named by retained state or by a possibly published replacement.
    std::vector<ArchiveRef> referenced;
    /// Configured retention target; zero disables cleanup.
    load::MemoryRetention policy;
};

/**
 * Authoritative session storage.
 *
 * Implementations preserve recovery evidence rather than minimizing disk use:
 * they never report a damaged store as empty, never reopen a stale fallback and
 * never rewrite a settled archive.
 */
class StateStore {
public:
    virtual ~StateStore();

    /// Backend that owns this session's authoritative state.
    virtual Backend backend() const noexcept = 0;

    /// Session identity every record and archive in this store belongs to.
    virtual const std::string& session_id() const noexcept = 0;

    /**
     * Load the authoritative current state.
     *
     * Returns nothing only when no store exists yet. A corrupt or unreadable
     * store throws StoreError instead of reporting an empty session, because a
     * damaged session must not look fresh. Loading is read-only: it creates no
     * authoritative database or schema, starts no loop and rebuilds no runtime resource. The worker
     * reconciles session identity after loading, so a snapshot written by another
     * session is rejected instead of being adopted silently.
     */
    virtual std::optional<StoredState> load_current() = 0;

    /**
     * Publish a complete state at a required boundary.
     *
     * The caller passes its live state and must not mutate it until the call
     * returns. A Committed receipt means the new state is durable at that
     * boundary. Any thrown StoreError is a required-write failure: the caller
     * stops writing and keeps recovery evidence, and an Unknown outcome is
     * never retried; the next startup's read-only load decides the survivor.
     *
     * A backend that reports revisions enforces expected_revision: a write
     * derived from a stale read fails instead of overwriting a newer state.
     * A backend that reports none rejects a non-empty expectation as a
     * programming error rather than pretending to validate it.
     *
     * boundary names the protocol checkpoint. Backends that keep no
     * per-boundary metadata, including JSON, validate and ignore it.
     *
     * The worker validates that the state belongs to this session before calling
     * a required checkpoint. A backend that persists session records must enforce
     * the same rule when it opens them.
     */
    virtual CommitReceipt checkpoint(
        const model_io::AgentInputState& state, Boundary boundary,
        std::optional<Revision> expected_revision = std::nullopt) = 0;

    /**
     * Reserve a never-reused archive identity without publishing anything.
     *
     * Preflight calls this before it accepts an attempt, so a rejected attempt
     * creates nothing while a failed attempt still consumes its ordinal and can
     * never reorder archives. Only the payload locator may be used before
     * begin_archive(); the archive does not exist yet.
     */
    virtual ArchiveRef reserve_archive(const CompactAttempt& attempt) = 0;

    /**
     * Freeze the pre-compaction original state as an archived attempt.
     *
     * Called after preflight accepts the attempt and before summarization, so the
     * attempt is visible while the model is suspended. The caller passes the
     * original state for this attempt, not the last checkpoint, which may be
     * stale when ordinary saves are disabled.
     *
     * SQLite also synchronizes the authoritative current graph in this same
     * transaction, so a crash during summarization restores unsaved original
     * edits. Its receipt carries the new revision for T2; expected_revision guards
     * against a stale original. JSON preserves its existing archive-only behavior
     * and returns no revision. Publication remains mandatory before model I/O.
     *
     * The payload is the complete state, exactly as it would be checkpointed. A
     * failed publication keeps the reserved identity: the attempt still consumes
     * its ordinal, the archive directory is not reused, and a payload that was
     * renamed into place before a directory-sync failure is reported as Unknown
     * and left as evidence. Retention ignores such a candidate until the archive
     * is complete and recognized.
     */
    virtual CommitReceipt begin_archive(
        const ArchiveRef& archive, const model_io::AgentInputState& original,
        std::optional<Revision> expected_revision = std::nullopt) = 0;

    /**
     * Publish a compact replacement and keep its archive immutable.
     *
     * Revision and outcome rules match checkpoint(). Success is reported only
     * after the replacement is durable, so a caller may then replace live state,
     * bump the history revision and announce success. This call never rewrites
     * the archive.
     */
    virtual CommitReceipt commit_compaction(
        const ArchiveRef& archive, const model_io::AgentInputState& replacement,
        std::optional<Revision> expected_revision = std::nullopt) = 0;

    /**
     * Record that an attempt ended without a committed replacement.
     *
     * Success is never reported here: commit_compaction() settles its own
     * attempt, so a backend cannot turn a durable commit into a failure by
     * failing a later bookkeeping call. The JSON layout encodes failure,
     * cancellation and interruption implicitly (the archive holds exactly the
     * frozen original state and is never rewritten, while the authoritative
     * snapshot still holds the original conversation), so the adapter validates
     * ownership and returns. A database backend stores the attempt state here for
     * restart recovery. Callers on unwind paths must contain exceptions; the
     * settled primary diagnostic wins.
     */
    virtual void finish_archive_attempt(
        const ArchiveRef& archive, ArchiveOutcome outcome) = 0;

    /**
     * Forget a reservation that never reached begin_archive().
     *
     * Preflight can reject an attempt after reserve_archive() succeeded. The
     * attempt then owns no archive and no settlement applies, but a backend that
     * keeps reservations in memory (SQLite) must drop the entry instead of
     * growing for the process lifetime. Releasing an unknown, empty or foreign
     * reference is a no-op: the hook exists for failure paths that must not
     * throw. Backends without reservations ignore it.
     */
    virtual void release_archive(const ArchiveRef&) {}

    /**
     * Apply the configured retention target after a settled attempt.
     *
     * Retention policy is unchanged by this abstraction: the current attempt and
     * the archives referenced by retained state are protected, other recognized
     * archives are kept newest first, and unknown files, symlinks and incomplete
     * archives stay untouched. Legacy Markdown artifacts are not candidates:
     * nothing can reconstruct their content, so they are preserved rather than
     * deleted. Cleanup errors are reported to the caller without changing an
     * already committed outcome.
     */
    virtual CleanupResult prune_archives(const ArchivePruneRequest& request) = 0;

    /**
     * Map a historical on-disk archive path recorded in retained state.
     *
     * Retained conversation state names the payload file of an earlier attempt,
     * and historical states may instead name the archive directory. Both resolve
     * to a recognized archive of this store. Retention uses this mapping only to
     * keep those archives protected; nothing is read, created or returned to a
     * client. A path this store does not own, including a nested or foreign one,
     * resolves to nothing. A backend without filesystem archives returns nothing,
     * because it has no path-based protection to preserve.
     */
    virtual std::optional<ArchiveRef> archive_at(
        const std::filesystem::path& payload_or_directory) const = 0;
};

/** Construction inputs for the JSON backend. */
struct JsonStoreOptions {
    /**
     * Directory holding the authoritative snapshot. A relative path resolves
     * against the current directory, matching the worker configuration.
     */
    std::filesystem::path state_directory;
    /**
     * Compact-archive root (`persistence.memory`). Archive operations require a
     * non-empty root and fail without one, exactly as the previous worker code
     * did; state saves and restoration never touch it.
     */
    std::filesystem::path archive_root;
};

/**
 * Create the JSON backend for one session.
 *
 * The store preserves the worker's file-based layout and behavior: `state.json`
 * is replaced atomically through the dataclass codec, and each compact attempt is
 * frozen as `<archive-root>/<archive-id>/state.json` plus a `manifest.json`
 * publication marker. Retention recognizes complete archives of that shape and
 * leaves legacy `state.md` artifacts, unknown files, symlinks and incomplete
 * archives alone. Construction performs no I/O.
 */
std::unique_ptr<StateStore> make_json_store(
    std::string session_id, JsonStoreOptions options);

/**
 * Wall-clock metadata for persistence records (UTC, second precision).
 * Ordering and deadlines use other mechanisms. Throws when the C library cannot
 * format the current time.
 */
std::string utc_timestamp();

} // namespace persistence

namespace persistence {
/** Create a normalized SQLite store. Construction performs no I/O.
 * Current snapshots and immutable compact archives share the same database.
 * Missing files are absent on load, never created by inspection.
 */
std::unique_ptr<StateStore> make_sqlite_store(
    std::string session_id, std::filesystem::path state_directory);
}
