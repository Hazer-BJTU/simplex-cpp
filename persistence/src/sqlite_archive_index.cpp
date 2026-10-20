#include "sqlite_archive_index.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace persistence::sqlite_archive_index {
namespace {

constexpr std::size_t chunk_bytes = 4096;
// A query has at most 256 scalars, but case-equivalent source scalars may
// occupy four bytes each (for example ASCII k matches the Kelvin sign).
constexpr std::size_t overlap_bytes = 256 * 4;

/** Return the length of a valid UTF-8 scalar, or zero for an invalid byte. */
std::size_t scalar_size(std::string_view text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return 1;
    const std::size_t size = first >= 0xc2 && first <= 0xdf ? 2
        : first >= 0xe0 && first <= 0xef ? 3
        : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (size == 0 || offset + size > text.size()) return 0;
    for (std::size_t i = 1; i < size; ++i) {
        const auto byte = static_cast<unsigned char>(text[offset + i]);
        if (byte < 0x80 || byte > 0xbf) return 0;
    }
    const auto second = static_cast<unsigned char>(text[offset + 1]);
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0)
        || (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) {
        return 0;
    }
    return size;
}

/** Sanitize only the search projection. Replacing each invalid byte and NUL by
 * one ASCII space keeps every byte offset valid against the original source. */
std::string searchable(std::string text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto size = scalar_size(text, offset);
        if (size == 0 || text[offset] == '\0') {
            text[offset++] = ' ';
        } else {
            offset += size;
        }
    }
    return text;
}

bool continuation(char byte) {
    return (static_cast<unsigned char>(byte) & 0xc0) == 0x80;
}

} // namespace

const char* schema_sql() {
    return R"sql(
-- A source identifier is scoped by its random, never-reused archive identity.
-- Row ids alone are not public identities: SQLite can reuse them after pruning.
CREATE VIEW archive_text_sources AS
WITH owners AS NOT MATERIALIZED (
  SELECT a.archive_id, m.message_id, m.slot,
         COALESCE(t.ordinal, st.ordinal) AS turn_index,
         s.ordinal AS step_index
  FROM archives a
  JOIN snapshots sn ON sn.snapshot_id = a.original_snapshot_id
    AND sn.kind = 'archive' AND sn.session_id = a.session_id
  JOIN messages m ON m.snapshot_id = sn.snapshot_id
  LEFT JOIN turns t ON t.turn_id = m.turn_id
  LEFT JOIN agent_steps s ON s.step_id = m.step_id
  LEFT JOIN turns st ON st.turn_id = s.turn_id
)
SELECT o.archive_id, 'message:' || o.message_id || ':body:' || c.ordinal AS source_id,
       o.turn_index, o.step_index,
       CASE o.slot WHEN 'user_input' THEN 'user' WHEN 'model_response' THEN 'assistant'
         ELSE 'tool_body' END AS kind,
       c.raw AS body
FROM owners o JOIN message_contents c ON c.message_id = o.message_id
WHERE c.slot = 'body' AND c.type = 'text'
UNION ALL
SELECT o.archive_id, 'message:' || o.message_id || ':call:' || i.ordinal,
       o.turn_index, o.step_index, 'tool_call', i.name || char(10) || i.arguments_json
FROM owners o JOIN message_invokes i ON i.message_id = o.message_id
UNION ALL
SELECT o.archive_id, 'message:' || o.message_id || ':return_call',
       o.turn_index, o.step_index, 'tool_call', i.query_name || char(10) || i.query_arguments_json
FROM owners o JOIN message_invoke_returns i ON i.message_id = o.message_id
UNION ALL
SELECT o.archive_id, 'message:' || o.message_id || ':result',
       o.turn_index, o.step_index, 'tool_result', i.output_raw
FROM owners o JOIN message_invoke_returns i ON i.message_id = o.message_id
WHERE i.output_type = 'text'
UNION ALL
SELECT a.archive_id, 'pending:' || p.ordinal || ':call',
       NULL, NULL, 'pending_tool_call', p.query_name || char(10) || p.query_arguments_json
FROM archives a JOIN pending_results p ON p.snapshot_id = a.original_snapshot_id
UNION ALL
SELECT a.archive_id, 'pending:' || p.ordinal || ':result',
       NULL, NULL, 'pending_tool_result', p.output_raw
FROM archives a JOIN pending_results p ON p.snapshot_id = a.original_snapshot_id
WHERE p.output_type = 'text';

CREATE TABLE archive_chunks (
  chunk_id INTEGER PRIMARY KEY,
  archive_id TEXT NOT NULL REFERENCES archives(archive_id) ON DELETE CASCADE,
  source_id TEXT NOT NULL,
  turn_index INTEGER,
  step_index INTEGER,
  kind TEXT NOT NULL,
  start_byte INTEGER NOT NULL CHECK(start_byte >= 0),
  end_byte INTEGER NOT NULL CHECK(end_byte > start_byte),
  source_bytes INTEGER NOT NULL CHECK(source_bytes >= end_byte),
  body TEXT NOT NULL,
  UNIQUE(archive_id, source_id, start_byte)
) STRICT;
CREATE VIRTUAL TABLE archive_chunks_fts USING fts5(
  body, content='archive_chunks', content_rowid='chunk_id',
  tokenize='trigram', detail='full'
);
CREATE TRIGGER archive_chunks_insert AFTER INSERT ON archive_chunks BEGIN
  INSERT INTO archive_chunks_fts(rowid, body) VALUES(new.chunk_id, new.body);
END;
CREATE TRIGGER archive_chunks_delete AFTER DELETE ON archive_chunks BEGIN
  INSERT INTO archive_chunks_fts(archive_chunks_fts, rowid, body)
    VALUES('delete', old.chunk_id, old.body);
END;
CREATE TRIGGER archive_chunks_update AFTER UPDATE ON archive_chunks BEGIN
  INSERT INTO archive_chunks_fts(archive_chunks_fts, rowid, body)
    VALUES('delete', old.chunk_id, old.body);
  INSERT INTO archive_chunks_fts(rowid, body) VALUES(new.chunk_id, new.body);
END;
UPDATE store_meta SET store_schema_version = 2 WHERE id = 1;
)sql";
}

void index_archive(sqlite::Database& database, std::string_view archive_id) {
    auto sources = database.prepare(
        "SELECT source_id, turn_index, step_index, kind, body "
        "FROM archive_text_sources WHERE archive_id = ?1");
    sources.bind_text(1, archive_id);
    while (sources.step() == sqlite::StepResult::Row) {
        const auto text = searchable(sources.column_text(4));
        for (std::size_t start = 0; start < text.size();) {
            auto end = std::min(start + chunk_bytes, text.size());
            while (end < text.size() && continuation(text[end])) --end;
            // Prefer a nearby paragraph/line boundary without producing tiny chunks.
            if (end < text.size()) {
                const auto newline = text.rfind('\n', end - 1);
                if (newline != std::string::npos && newline >= start + chunk_bytes / 2) {
                    end = newline + 1;
                }
            }
            auto insert = database.prepare(
                "INSERT INTO archive_chunks(archive_id, source_id, turn_index, step_index, "
                "kind, start_byte, end_byte, source_bytes, body) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
            insert.bind_text(1, archive_id);
            insert.bind_text(2, sources.column_text(0));
            if (sources.column_is_null(1)) insert.bind_null(3);
            else insert.bind_int64(3, sources.column_int64(1));
            if (sources.column_is_null(2)) insert.bind_null(4);
            else insert.bind_int64(4, sources.column_int64(2));
            insert.bind_text(5, sources.column_text(3));
            insert.bind_int64(6, static_cast<std::int64_t>(start));
            insert.bind_int64(7, static_cast<std::int64_t>(end));
            insert.bind_int64(8, static_cast<std::int64_t>(text.size()));
            insert.bind_text(9, std::string_view(text).substr(start, end - start));
            if (insert.step() != sqlite::StepResult::Done) {
                throw std::runtime_error("archive chunk insertion did not complete");
            }
            if (end == text.size()) break;
            start = end - overlap_bytes;
            while (continuation(text[start])) --start;
        }
    }
}

void backfill(sqlite::Database& database) {
    auto archives = database.prepare(
        "SELECT archive_id FROM archives WHERE storage_kind = 'sqlite' ORDER BY sequence");
    while (archives.step() == sqlite::StepResult::Row) {
        index_archive(database, archives.column_text(0));
    }
}

void inspect(sqlite::Database& database) {
    for (const auto* name : {"archive_text_sources", "archive_chunks", "archive_chunks_fts",
             "archive_chunks_insert", "archive_chunks_delete", "archive_chunks_update"}) {
        auto query = database.prepare("SELECT 1 FROM sqlite_master WHERE name = ?1");
        query.bind_text(1, name);
        if (query.step() != sqlite::StepResult::Row) {
            throw std::runtime_error("SQLite archive search schema is incomplete");
        }
    }
}

} // namespace persistence::sqlite_archive_index
