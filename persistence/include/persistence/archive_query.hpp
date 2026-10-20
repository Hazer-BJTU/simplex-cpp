#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

namespace persistence {

/** Query failures never imply an authoritative write or change its outcome. */
enum class ArchiveQueryFailure {
    MissingStore,
    IndexUnavailable,
    NotFound,
    Cancelled,
    TimedOut,
    InvalidArgument,
    StorageError,
};

/** Public diagnostics omit SQL, search text and archived contents. */
class ArchiveQueryError : public std::runtime_error {
public:
    ArchiveQueryError(ArchiveQueryFailure reason, const std::string& message);
    ~ArchiveQueryError() override;
    ArchiveQueryFailure reason() const noexcept { return reason_; }
private:
    ArchiveQueryFailure reason_;
};

/** Attempt outcome, distinct from the success/failure of individual tools. */
enum class ArchivedAttemptStatus { Prepared, Succeeded, Failed, Cancelled, Interrupted };

/** Searchable projections; reasoning, extras, prompts and binary data are excluded. */
enum class ArchiveTextKind {
    User, Assistant, ToolBody, ToolCall, ToolResult, PendingToolCall, PendingToolResult,
};

/** Identity and lifecycle metadata of one retained, complete original snapshot. */
struct ArchiveInfo {
    std::string archive_id;
    std::int64_t sequence = 0;
    std::string created_at;
    ArchivedAttemptStatus outcome = ArchivedAttemptStatus::Prepared;
    bool automatic = false;
};

/** Per-call cooperative budget. Range: 1..30000 ms. Does not bound OS filesystem
 * stalls; SQLite VM work and application loops check it. No partial result is
 * returned on timeout/cancellation. The owning connection is discarded. */
struct ArchiveQueryOptions {
    std::chrono::milliseconds timeout{1000};
    std::stop_token stop;
};

/** Newest-first archive page. Use the last sequence as the next before_sequence.
 * Each call observes one read transaction; retention may remove rows between calls. */
struct ArchivePage {
    std::vector<ArchiveInfo> archives;
    bool has_more = false;
};

/** Literal substring query, not SQL or FTS expression syntax. Text must be valid
 * UTF-8, contain no NUL, and occupy 1..256 bytes with a non-whitespace character.
 * Three or more Unicode scalars use case-insensitive FTS5 trigram matching.
 * One/two scalars use an exact, case-sensitive scan under the same budget.
 * At most 256 query scalars can match up to 1024 source bytes after case
 * folding; chunk overlap covers that conservative bound.
 */
struct ArchiveSearchRequest {
    std::string text;
    std::optional<std::string> archive_id;
    std::optional<ArchiveTextKind> kind;
    std::size_t limit = 10;          ///< 1..100 distinct hits.
    std::size_t snippet_bytes = 512; ///< 32..4096 UTF-8 bytes per preview.
};

/** A source locator is scoped by archive_id; never persist a chunk rowid alone.
 * Offsets address the source projection in bytes. ToolCall projects name + '\n'
 * + serialized arguments. Other kinds project the corresponding original text.
 */
struct ArchiveHit {
    ArchiveInfo archive;
    std::string source_id;
    ArchiveTextKind kind = ArchiveTextKind::User;
    std::optional<std::int64_t> turn;
    std::optional<std::int64_t> step;
    std::uint64_t start_byte = 0;
    std::uint64_t end_byte = 0;
    /// Source-text preview; contains the complete first match when it fits the
    /// byte budget, including case-equivalent spellings with different lengths.
    std::string snippet;
    /// Some source text was omitted; this also covers an oversized matched span.
    bool truncated = false;
};

/** Best-first hits (BM25, then newest archive and source position). Exact repeated
 * chunks of the same kind and overlapping chunks of a source are collapsed.
 * has_more means another distinct hit was observed, not a total-match count. */
struct ArchiveSearchResult {
    std::vector<ArchiveHit> hits;
    bool has_more = false;
};

/** A bounded slice of the original source, including any original NUL/invalid
 * bytes. No normalization is applied here. end_byte is the next offset to read.
 * Valid UTF-8 scalars are not split; a start inside a valid multibyte scalar is rejected.
 */
struct ArchiveExcerpt {
    std::string text;
    std::uint64_t start_byte = 0;
    std::uint64_t end_byte = 0;
    std::uint64_t total_bytes = 0;
    bool has_more = false;
};

/**
 * Session-scoped archive reading, independent of StateStore's write contract.
 * Calls are synchronous: use a storage executor, never the network event loop.
 * Each call opens its own read-only connection/transaction and validates session
 * identity. The reader is immutable and may be called concurrently. It neither
 * creates/migrates storage nor pins archives against retention. A read after
 * pruning returns NotFound; archive ids cannot alias later reused SQLite rowids.
 */
class ArchiveReader {
public:
    virtual ~ArchiveReader();
    virtual ArchivePage list(std::size_t limit = 20,
        std::optional<std::int64_t> before_sequence = {},
        const ArchiveQueryOptions& options = {}) const = 0;
    virtual ArchiveSearchResult search(const ArchiveSearchRequest& request,
        const ArchiveQueryOptions& options = {}) const = 0;
    /** Read 4..65536 requested bytes. offset must be within the source; an offset
     * equal to its size returns an empty terminal slice. Unknown ids are NotFound. */
    virtual ArchiveExcerpt read(const std::string& archive_id, const std::string& source_id,
        std::uint64_t offset = 0, std::size_t bytes = 16384,
        const ArchiveQueryOptions& options = {}) const = 0;
};

/** Construct a reader without I/O. Only SQLite is supported; no JSON fallback.
 * Missing files fail at query time without creating anything. Schema v1 current
 * state remains readable, but archive queries report IndexUnavailable until the
 * owning writer upgrades/backfills during its next normal store write. */
std::unique_ptr<ArchiveReader> make_sqlite_archive_reader(
    std::string session_id, std::filesystem::path state_directory);

} // namespace persistence
