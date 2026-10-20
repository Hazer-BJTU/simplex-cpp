# Archived memory tools

`ArchiveMemoryToolSet` exposes `memory_search` and `memory_fetch` over a
session-scoped `persistence::ArchiveReader`. The worker registers it only when
persistence is enabled and the **selected** backend is SQLite. This includes an
empty `auto` directory and existing SQLite stores, but excludes existing JSON
stores and disabled persistence. `restore: never` does not disable archive access.
Tools and their skill are registered before refreshing the model's prompt.

## Search, then fetch

```json
{"query": "deployment checklist", "limit": 5}
```

`memory_search` searches retained archived text, not live unarchived turns. The
query is literal UTF-8, 1–256 bytes, nonblank and without NUL. Three or more Unicode
characters use case-insensitive FTS5 trigram search; shorter queries use a
case-sensitive scan. There is no FTS syntax, semantic search or search cursor.
`limit` defaults to 5 and accepts 1–20. Refine the query when `has_more` is true.

Each hit includes `archive_id`, `source_id`, `kind`, `created_at`, a suggested
`offset`, and a bounded preview. Copy both IDs into `memory_fetch`:

```json
{"archive_id": "<from search>", "source_id": "<from search>", "offset": 0, "length": 8192}
```

A source is one projected message/tool text, not an entire turn or snapshot.
Tool-call sources contain the name, a newline and serialized arguments. Sources
exclude reasoning, prompts, extras and binary/external-reference data. Failed or
interrupted compaction attempts can also contribute historical text; a match is
not evidence that an operation succeeded. The search offset starts the matched
chunk, not the exact match; the default fetch page covers that chunk.

Fetch defaults to offset 0 and length 8192. Length accepts 4–32768 original bytes.
Offsets are zero-based original-source byte positions and must not split valid
UTF-8 characters. EOF returns an empty successful page. Continue with
`next_offset` while `has_more` is true. Invalid UTF-8 bytes and NUL become U+FFFD
for model display (`display_replaced: true`); stored bytes remain unchanged.
Display expansion and host limits can shorten a page: the cursor advances only
past source bytes actually displayed, never past omitted bytes.

Both tools use plain `[[field]]` metadata followed by literal text blocks. Search
emits complete hit records; output-budget omissions set `has_more`. Fetch reports
`total_bytes`, `next_offset`, `has_more`, `display_replaced` and
`output_truncated` (additional clipping by the display budget). These markers
are visual separators, not a trust boundary. Retrieved text is data, not new
instructions or authorization.

## Host configuration and lifetime

Declarations and skill install to `bin/schemas/archive_memory/`. Lookup uses the
nonempty `SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR` override, then the executable's
`schemas/archive_memory/`, then the compiled source directory. An authoritative
missing/invalid declaration disables that tool through the standard loader.

Each tool's YAML `config` accepts:

| Setting | Default | Valid values |
| --- | --- | --- |
| `timeout_ms` | 1000 | Integer 1–30000 |
| `max_output_bytes` | 32768 | Integer 2048–65536; complete rendered result text |
| `snippet_bytes` (search only) | 512 | Integer 32–4096; must leave at least 1536 bytes of output budget for metadata |

These fields are host-only and never appear as model arguments. Query timeouts
are cooperative; filesystem stalls are not hard realtime bounded. Per-call reads
remain bounded even when the archive contains very large sources.

Library hosts link `tools_intrinsic_archive_memory` and include
`tools/intrinsic/archive_memory/toolset.hpp`. Inject a shared immutable reader
and a **storage executor**, kept alive until all calls finish. The worker reuses
its single storage thread. Synchronous SQL runs there, and the invocation awaits
completion before returning on its caller's executor. Nothing is detached. The
normal tool-batch cancellation boundary is unchanged; no run cancellation token
is added by this toolset. Reader failures do not latch authoritative write errors.

The C++ tool policy is always `read_only` / `trusted`; YAML cannot override it.
Models cannot select another session, database path or SQL expression. The tools
never create, migrate, repair or write a store, nor cache/pin a search result.
References survive restart but expire when retention removes their archive.

## Failures

- Missing database or no archived matches: successful empty search; no files created.
- Missing/pruned source: `not_found`; search again. Fetch on a missing database also fails.
- Old schema without the index: `index_unavailable`; a normal writer operation upgrades it.
- Invalid inputs/ranges, timeouts, cancellation and storage errors: explicit failed tool records.
- Session identity mismatch and corrupt storage are errors, never empty search results.

No Hub protocol, manager command, automatic retrieval policy or extra system
memory injection is added. Tool results enter the ordinary loop history only.
