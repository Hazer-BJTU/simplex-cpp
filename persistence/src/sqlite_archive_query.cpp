#include "persistence/archive_query.hpp"

#include "sqlite_schema.hpp"
#include "sqlite/database.hpp"

#include <algorithm>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>

namespace persistence {
namespace {

[[noreturn]] void invalid(const char* message) {
    throw ArchiveQueryError(ArchiveQueryFailure::InvalidArgument, message);
}

bool continuation(char value) {
    return (static_cast<unsigned char>(value) & 0xc0) == 0x80;
}

/** Length of a valid scalar at offset; malformed bytes are not boundaries to
 * protect. This permits lossless paging even through legacy invalid text. */
std::size_t scalar_size(std::string_view text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    const std::size_t size = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
        : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (size == 0 || offset + size > text.size()) return 0;
    for (std::size_t i = 1; i < size; ++i) {
        if (!continuation(text[offset + i])) return 0;
    }
    if (size > 1) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0)
            || (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return 0;
    }
    return size;
}

/** Return the scalar start when a requested cut lies inside valid UTF-8. */
std::size_t boundary_before(std::string_view text, std::size_t cut) {
    for (auto i = cut > 3 ? cut - 3 : 0; i < cut; ++i) {
        if (i + scalar_size(text, i) > cut) return i;
    }
    return cut;
}

/** Count scalars and reject invalid UTF-8 before constructing an FTS phrase. */
std::size_t validate_text(std::string_view text) {
    if (text.empty() || text.size() > 256 || text.find('\0') != std::string_view::npos
        || text.find_first_not_of(" \t\n\r\f\v") == std::string_view::npos) {
        invalid("archive query requires 1..256 bytes of nonblank UTF-8 text without NUL");
    }
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < text.size(); ++count) {
        const auto first = static_cast<unsigned char>(text[offset]);
        const std::size_t size = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
            : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (size == 0 || offset + size > text.size()) invalid("archive query is not valid UTF-8");
        for (std::size_t i = 1; i < size; ++i) {
            if (!continuation(text[offset + i])) invalid("archive query is not valid UTF-8");
        }
        if (size > 1) {
            const auto second = static_cast<unsigned char>(text[offset + 1]);
            if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0)
                || (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) {
                invalid("archive query is not valid UTF-8");
            }
        }
        offset += size;
    }
    return count;
}

void validate_id(std::string_view id) {
    if (id.empty() || id.size() > 256 || id.find('\0') != std::string_view::npos) {
        invalid("archive/source identity must contain 1..256 bytes without NUL");
    }
}

const char* kind_name(ArchiveTextKind kind) {
    switch (kind) {
    case ArchiveTextKind::User: return "user";
    case ArchiveTextKind::Assistant: return "assistant";
    case ArchiveTextKind::ToolBody: return "tool_body";
    case ArchiveTextKind::ToolCall: return "tool_call";
    case ArchiveTextKind::ToolResult: return "tool_result";
    case ArchiveTextKind::PendingToolCall: return "pending_tool_call";
    case ArchiveTextKind::PendingToolResult: return "pending_tool_result";
    }
    invalid("invalid archive text kind");
}

ArchiveTextKind parse_kind(std::string_view name) {
    for (const auto kind : {ArchiveTextKind::User, ArchiveTextKind::Assistant,
             ArchiveTextKind::ToolBody, ArchiveTextKind::ToolCall, ArchiveTextKind::ToolResult,
             ArchiveTextKind::PendingToolCall, ArchiveTextKind::PendingToolResult}) {
        if (name == kind_name(kind)) return kind;
    }
    throw std::runtime_error("invalid stored archive text kind");
}

ArchiveInfo archive_info(sqlite::Statement& row) {
    ArchiveInfo result;
    result.archive_id = row.column_text(0);
    result.sequence = row.column_int64(1);
    result.created_at = row.column_text(2);
    const auto outcome = row.column_text(3);
    if (outcome == "prepared") result.outcome = ArchivedAttemptStatus::Prepared;
    else if (outcome == "succeeded") result.outcome = ArchivedAttemptStatus::Succeeded;
    else if (outcome == "failed") result.outcome = ArchivedAttemptStatus::Failed;
    else if (outcome == "cancelled") result.outcome = ArchivedAttemptStatus::Cancelled;
    else if (outcome == "interrupted") result.outcome = ArchivedAttemptStatus::Interrupted;
    else throw std::runtime_error("invalid stored archive outcome");
    const auto mode = row.column_text(4);
    if (mode != "manual" && mode != "automatic") throw std::runtime_error("invalid stored archive mode");
    result.automatic = mode == "automatic";
    return result;
}

/** Locate the first highlighted source span using SQLite's actual tokenizer
 * matches, including Unicode case folding. Both markers are checked against
 * this bounded chunk before use, so archived text cannot impersonate a marker.
 * Only accepted hits incur this extra lookup, in the same read transaction.
 * highlight() may merge overlapping matches; keeping the beginning of that
 * merged span still preserves its first complete match whenever it fits.
 */
std::pair<std::size_t, std::size_t> first_fts_match(
    sqlite::Database& database, std::int64_t chunk_id, std::string_view phrase,
    std::string_view body, sqlite::ExecutionBudget& budget) {
    std::string opening;
    std::string closing;
    for (std::size_t candidate = 0;; ++candidate) {
        budget.check();
        const auto prefix = "<simplex-match-" + std::to_string(candidate);
        opening = prefix + "-open>";
        closing = prefix + "-close>";
        if (body.find(opening) == std::string_view::npos
            && body.find(closing) == std::string_view::npos) {
            break;
        }
    }
    auto match = database.prepare(
        "SELECT highlight(archive_chunks_fts, 0, ?2, ?3) FROM archive_chunks_fts "
        "WHERE rowid = ?1 AND archive_chunks_fts MATCH ?4");
    match.bind_int64(1, chunk_id);
    match.bind_text(2, opening);
    match.bind_text(3, closing);
    match.bind_text(4, phrase);
    if (match.step() != sqlite::StepResult::Row) {
        throw std::runtime_error("archive match disappeared within its read transaction");
    }
    budget.check();
    const auto highlighted = match.column_text(0);
    const auto begin = highlighted.find(opening);
    if (begin == std::string::npos) {
        throw std::runtime_error("archive highlight has no matching span");
    }
    const auto marked_begin = begin + opening.size();
    const auto marked_end = highlighted.find(closing, marked_begin);
    if (marked_end == std::string::npos || begin > body.size()
        || marked_end - marked_begin > body.size() - begin) {
        throw std::runtime_error("archive highlight has an invalid matching span");
    }
    return {begin, begin + marked_end - marked_begin};
}

/** Select context in source bytes, preserving the entire matched span whenever
 * it fits. Long/merged spans start at their first match and are explicitly
 * clipped by the caller's truncation flag. No ellipses consume the byte budget.
 * Both edges lie on UTF-8 boundaries in the normalized, valid-UTF-8 chunk.
 */
std::string match_preview(std::string_view body, std::size_t begin,
                          std::size_t end, std::size_t byte_limit) {
    const auto match_bytes = end - begin;
    const auto context = match_bytes < byte_limit ? (byte_limit - match_bytes) / 2 : 0;
    auto start = begin > context ? begin - context : 0;
    while (start < begin && continuation(body[start])) ++start;
    const auto finish = boundary_before(body, std::min(body.size(), start + byte_limit));
    return std::string(body.substr(start, finish - start));
}

/** Own no connections between calls. A failed/cancelled query cannot poison a
 * writer or leave a read transaction blocking WAL checkpointing. */
class SqliteArchiveReader final : public ArchiveReader {
public:
    SqliteArchiveReader(std::string session, std::filesystem::path directory)
        : session_(std::move(session)), path_(std::move(directory) / "state.sqlite3") {
        if (session_.empty()) invalid("archive reader requires a session identity");
    }

    ArchivePage list(std::size_t limit, std::optional<std::int64_t> before,
                     const ArchiveQueryOptions& options) const override {
        if (limit == 0 || limit > 100 || (before && *before <= 0)) {
            invalid("archive page requires limit 1..100 and a positive sequence cursor");
        }
        return query(options, [&](sqlite::Database& db, sqlite::ExecutionBudget& budget) {
            auto rows = db.prepare(
                "SELECT archive_id, sequence, created_at, outcome, mode FROM archives "
                "WHERE session_id = ?1 AND storage_kind = 'sqlite' "
                "AND (?2 IS NULL OR sequence < ?2) ORDER BY sequence DESC LIMIT ?3");
            rows.bind_text(1, session_);
            if (before) rows.bind_int64(2, *before);
            else rows.bind_null(2);
            rows.bind_int64(3, static_cast<std::int64_t>(limit + 1));
            ArchivePage result;
            while (rows.step() == sqlite::StepResult::Row) {
                budget.check();
                if (result.archives.size() == limit) {
                    result.has_more = true;
                    break;
                }
                result.archives.push_back(archive_info(rows));
            }
            return result;
        });
    }

    ArchiveSearchResult search(const ArchiveSearchRequest& request,
                              const ArchiveQueryOptions& options) const override {
        const bool fts = validate_text(request.text) >= 3;
        if (request.limit == 0 || request.limit > 100
            || request.snippet_bytes < 32 || request.snippet_bytes > 4096) {
            invalid("archive search requires limit 1..100 and snippet_bytes 32..4096");
        }
        if (request.archive_id) validate_id(*request.archive_id);
        if (request.kind) (void)kind_name(*request.kind);
        std::string phrase = "\"";
        for (char byte : request.text) {
            if (byte == '"') phrase += '"';
            phrase += byte;
        }
        phrase += '"';
        return query(options, [&](sqlite::Database& db, sqlite::ExecutionBudget& budget) {
            std::string sql =
                "SELECT a.archive_id, a.sequence, a.created_at, a.outcome, a.mode, "
                "c.source_id, c.kind, c.turn_index, c.step_index, c.start_byte, c.end_byte, "
                "c.body, c.source_bytes, c.chunk_id "
                "FROM archive_chunks c JOIN archives a ON a.archive_id = c.archive_id ";
            if (fts) {
                sql += "JOIN archive_chunks_fts ON archive_chunks_fts.rowid = c.chunk_id ";
            }
            sql += "WHERE a.session_id = ?1 AND (?2 IS NULL OR a.archive_id = ?2) "
                "AND (?3 IS NULL OR c.kind = ?3) AND ";
            sql += fts ? "archive_chunks_fts MATCH ?4 " : "instr(c.body, ?4) > 0 ";
            sql += "ORDER BY ";
            if (fts) sql += "archive_chunks_fts.rank, ";
            sql += "a.sequence DESC, c.turn_index, c.step_index, c.source_id, c.start_byte";
            auto rows = db.prepare(sql);
            rows.bind_text(1, session_);
            if (request.archive_id) rows.bind_text(2, *request.archive_id);
            else rows.bind_null(2);
            if (request.kind) rows.bind_text(3, kind_name(*request.kind));
            else rows.bind_null(3);
            rows.bind_text(4, fts ? phrase : request.text);
            ArchiveSearchResult result;
            std::set<std::pair<ArchiveTextKind, std::string>> seen;
            while (rows.step() == sqlite::StepResult::Row) {
                budget.check();
                ArchiveHit hit;
                hit.archive = archive_info(rows);
                hit.source_id = rows.column_text(5);
                hit.kind = parse_kind(rows.column_text(6));
                if (!rows.column_is_null(7)) hit.turn = rows.column_int64(7);
                if (!rows.column_is_null(8)) hit.step = rows.column_int64(8);
                hit.start_byte = static_cast<std::uint64_t>(rows.column_int64(9));
                hit.end_byte = static_cast<std::uint64_t>(rows.column_int64(10));
                const auto body = rows.column_text(11);
                if (seen.contains({hit.kind, body})) continue;
                const bool overlaps = std::any_of(result.hits.begin(), result.hits.end(), [&](const auto& other) {
                    return other.archive.archive_id == hit.archive.archive_id
                        && other.source_id == hit.source_id && other.start_byte < hit.end_byte
                        && hit.start_byte < other.end_byte;
                });
                if (overlaps) continue;
                if (result.hits.size() == request.limit) {
                    result.has_more = true;
                    break;
                }
                seen.emplace(hit.kind, body);
                const auto span = fts
                    ? first_fts_match(db, rows.column_int64(13), phrase, body, budget)
                    : std::pair{body.find(request.text), body.find(request.text) + request.text.size()};
                hit.snippet = match_preview(body, span.first, span.second, request.snippet_bytes);
                hit.truncated = hit.start_byte != 0
                    || hit.end_byte < static_cast<std::uint64_t>(rows.column_int64(12))
                    || hit.snippet != body;
                result.hits.push_back(std::move(hit));
            }
            return result;
        });
    }

    ArchiveExcerpt read(const std::string& archive_id, const std::string& source_id,
                        std::uint64_t offset, std::size_t bytes,
                        const ArchiveQueryOptions& options) const override {
        validate_id(archive_id);
        validate_id(source_id);
        if (bytes < 4 || bytes > 65536 || offset > static_cast<std::uint64_t>(INT64_MAX - 1)) {
            invalid("archive read requires 4..65536 bytes and an addressable offset");
        }
        return query(options, [&](sqlite::Database& db, sqlite::ExecutionBudget& budget) {
            auto row = db.prepare(
                "SELECT length(CAST(body AS BLOB)), substr(CAST(body AS BLOB), ?3, ?4) "
                "FROM archive_text_sources WHERE archive_id = ?1 AND source_id = ?2");
            row.bind_text(1, archive_id);
            row.bind_text(2, source_id);
            const auto prefix = static_cast<std::size_t>(std::min<std::uint64_t>(offset, 3));
            row.bind_int64(3, static_cast<std::int64_t>(offset - prefix + 1));
            row.bind_int64(4, static_cast<std::int64_t>(prefix + bytes + 4));
            if (row.step() != sqlite::StepResult::Row) {
                throw ArchiveQueryError(ArchiveQueryFailure::NotFound, "archive source is absent or has been pruned");
            }
            budget.check();
            ArchiveExcerpt result;
            result.total_bytes = static_cast<std::uint64_t>(row.column_int64(0));
            if (offset > result.total_bytes) invalid("archive read offset exceeds source size");
            const auto fetched = row.column_text(1);
            if (boundary_before(fetched, prefix) != prefix) {
                invalid("archive read offset is inside a UTF-8 scalar");
            }
            const auto end = boundary_before(fetched, std::min(fetched.size(), prefix + bytes));
            result.text = fetched.substr(prefix, end - prefix);
            result.start_byte = offset;
            result.end_byte = offset + result.text.size();
            result.has_more = result.end_byte < result.total_bytes;
            return result;
        });
    }

private:
    template<class Function>
    auto query(const ArchiveQueryOptions& options, Function function) const
        -> std::invoke_result_t<Function, sqlite::Database&, sqlite::ExecutionBudget&> {
        if (options.timeout.count() < 1 || options.timeout > std::chrono::seconds(30)) {
            invalid("archive query timeout must be within 1..30000 milliseconds");
        }
        if (options.stop.stop_requested()) {
            throw ArchiveQueryError(ArchiveQueryFailure::Cancelled, "archive query cancelled");
        }
        try {
            if (!std::filesystem::exists(path_)) {
                throw ArchiveQueryError(ArchiveQueryFailure::MissingStore, "SQLite archive store does not exist");
            }
            sqlite_schema::require_regular_store_file(path_);
            sqlite::Database db(path_.string(), sqlite::OpenMode::ReadOnly);
            sqlite::ExecutionBudget budget(db, options.timeout, options.stop);
            try {
                sqlite::Transaction transaction(db);
                const auto identity = sqlite_schema::inspect_store(db, session_);
                if (identity.schema_version < 2) {
                    throw ArchiveQueryError(ArchiveQueryFailure::IndexUnavailable,
                        "archive index requires an upgrade by the owning writer");
                }
                auto result = function(db, budget);
                budget.check();
                return result;
            } catch (const sqlite::Error&) {
                if (budget.cancelled()) {
                    throw ArchiveQueryError(ArchiveQueryFailure::Cancelled, "archive query cancelled");
                }
                if (budget.expired()) {
                    throw ArchiveQueryError(ArchiveQueryFailure::TimedOut, "archive query timed out");
                }
                throw;
            }
        } catch (const ArchiveQueryError&) {
            throw;
        } catch (const sqlite::Error& error) {
            if (options.stop.stop_requested()) {
                throw ArchiveQueryError(ArchiveQueryFailure::Cancelled, "archive query cancelled");
            }
            // sqlite::Error includes only an operation and SQLite error code.
            throw ArchiveQueryError(ArchiveQueryFailure::StorageError,
                std::string("archive query failed: ") + error.what());
        } catch (const std::exception&) {
            // A cancellation can arrive while installing the budget itself.
            if (options.stop.stop_requested()) {
                throw ArchiveQueryError(ArchiveQueryFailure::Cancelled, "archive query cancelled");
            }
            throw ArchiveQueryError(ArchiveQueryFailure::StorageError,
                "archive query could not read the SQLite store");
        }
    }

    const std::string session_;
    const std::filesystem::path path_;
};

} // namespace

ArchiveQueryError::ArchiveQueryError(ArchiveQueryFailure reason, const std::string& message)
    : std::runtime_error(message), reason_(reason) {}
ArchiveQueryError::~ArchiveQueryError() = default;
ArchiveReader::~ArchiveReader() = default;

std::unique_ptr<ArchiveReader> make_sqlite_archive_reader(
    std::string session_id, std::filesystem::path state_directory) {
    return std::make_unique<SqliteArchiveReader>(std::move(session_id), std::move(state_directory));
}

} // namespace persistence
