# Authoritative session persistence

`persistence_lib` owns the worker's authoritative `AgentInputState` storage and
its compact-archive evidence. The contract is `<persistence/store.hpp>`; the JSON
adapter is `src/json_store.cpp`, and `src/json_archive.cpp` owns that adapter's
archive layout, recognition and retention. The SQLite adapter is `src/sqlite_store.cpp`, using the normalized
schema and row codec in `src/sqlite_schema.cpp` and `src/sqlite_snapshot.cpp`.

The module exists so that no execution code knows a storage format. `core` asks
for a checkpoint, a reserved archive, a compaction commit, or a retention pass;
it never names `state.json`, an archive directory, or a future database file.
Dataclass, loop, tool and provider code stay independent of the backend.

## Contract

- One store per session, one writer at a time, serialized by the host. The
  contract has no internal locking and no overlapping operations.
- Every operation is synchronous: it returns only after its durability outcome is
  known. A backend that needs a database writer still completes or fails the call
  before the worker continues, so a checkpoint can never be fire-and-forget.
- `checkpoint` and `commit_compaction` report `NotCommitted`, `Committed` or
  `Unknown`. `Unknown` (for JSON: the rename succeeded but the directory sync
  failed) stops further writes; the worker never retries it, and the next
  startup's read-only load decides which candidate survived. Codec and
  filesystem failures are translated into `StoreError`;
  programming errors such as a foreign reference stay standard exceptions.
- `reserve_archive` plans an identity during preflight; `begin_archive` freezes
  the original state before summarization; `commit_compaction` publishes the
  replacement and settles that attempt. `finish_archive_attempt` records only
  failure, cancellation or interruption, so a durable commit cannot be turned
  into a failure by later bookkeeping. A settled archive is never rewritten.
- `prune_archives` applies the configured retention target. The current attempt
  and archives named by retained state are protected; unknown files, symlinks and
  incomplete directories are untouched, and legacy Markdown archives are never
  candidates at all. `archive_at` maps a historical on-disk payload path to a
  store record for that protection only. Publication accepts a symlinked archive
  root, but retention requires an ordinary directory and refuses one: deletion
  never follows a link.
- The write contract has no query surface. The separate
  `<persistence/archive_query.hpp>` interface lists, searches and reads retained
  SQLite archives without changing retention policy.

## JSON adapter

The authoritative snapshot keeps the worker's long-standing file layout: a
required checkpoint atomically replaces `<state>/state.json` through
`load::save_state`, including its publication and directory-sync semantics. The
one intentional interface difference from the pre-#187 worker is the error type:
a required write reports a recovery outcome instead of a bare publication flag,
with the same message.

Each compact attempt is frozen below `<memory>/` as a directory named
`<20-digit ordinal>-<UTC timestamp>-<run_id>` containing:

```text
state.json      complete original AgentInputState (the restorable payload)
manifest.json   attempt identity, format version and payload size
```

The payload is written first through the same JSON codec; the manifest is
published atomically afterwards as the attempt's publication marker. Recognition
requires both files, an intact manifest and a matching payload size. Ordinals are
derived from existing entries and are never reused, including by failed attempts:
a failed publication keeps its reserved directory, and a payload renamed before a
directory-sync failure stays as `Unknown` evidence. A directory without its
manifest is incomplete, so nothing treats it as restorable state.

Archives written by earlier workers hold a single `state.md`. They are recognized
as `legacy_readable`: non-restorable evidence that retention never deletes,
because no worker can reconstruct or replace the Markdown it contains. Unknown
files, symlinks, extra files and incomplete archives are likewise untouched.

`readable.md` is no longer written. `persistence.readable` and the explicit
`load::StateFormat::Readable` export remain accepted for compatibility; the
worker reports one startup diagnostic and never treats either as authoritative
state. See `json_archive.cpp` for the layout and recognition rules.

## Build and validation

```sh
cmake --build build --target \
    test_persistence_store test_persistence_store_archive test_persistence_store_publication \
    test_persistence_sqlite_schema test_persistence_sqlite_snapshot
ctest --test-dir build -R 'persistence' --output-on-failure
```

`test_persistence_store` covers the backend-neutral contract, JSON layout, naming
and ownership rules. `test_persistence_store_archive` proves that an archive
round-trips the complete pre-compaction state (large text, binary content, loop
progress with an unprojected result batch and open extras), that recognition
rejects a foreign identity, a size mismatch or a missing manifest, that legacy
Markdown is preserved, and that retention keeps the current and referenced
archives newest first. `test_persistence_store_publication` interposes `fsync` in
the test executable to prove that a published-but-unsynced payload or manifest is
reported as `Unknown` and that an attempt without its marker is not recognized.

## SQLite schema v1

`src/sqlite_schema.cpp` holds the executable schema, its version and migration
guard; `src/sqlite_snapshot.cpp` is the normalized row codec;
`src/sqlite_store.cpp` implements the `StateStore`, checkpoint revisions and
transactional compaction attempts, while `src/json_store.cpp` keeps the JSON
backend. The owner-confirmed decision record is
[`docs/architecture/sqlite-schema-v1.md`](../docs/architecture/sqlite-schema-v1.md).

One database belongs to one session at `<state>/state.sqlite3` (D10). The store
path must be a regular file: a symlinked `state.sqlite3` is refused, and the file
mode is narrowed through the opened descriptor rather than the path. A snapshot
is a complete graph: every child row carries `snapshot_id`, and composite
foreign keys reject a cross-snapshot reference. `snapshots.kind = 'current'` is
unique per session through the partial index `snapshots_one_current`; archives
are `kind = 'archive'` and are only inserted, never updated.

### Opening and versions

- `open_store` enables `foreign_keys`, WAL and `synchronous=FULL`, creates schema
  v2 on an empty file in one transaction, and records the baseline in
  `schema_migrations`.
- `inspect_store` is read-only: it never creates, migrates or repairs. Schema v1 remains readable without migration. A newer
  schema, an unsupported codec, a foreign `application_id`, a missing
  `store_meta`/`sessions` row or a session mismatch throws `SchemaError` with a
  `Reason`.
- `apply_migrations` runs every newer DDL and its `schema_migrations` row in one
  transaction, so a failing migration leaves both the schema and the recorded
  version unchanged.

### Column mapping

**Snapshot header — `snapshots`**

| dataclass | column | encoding |
| --- | --- | --- |
| `meta.session_id` | `session_id` | must equal `sessions.session_id`; checked on write and read |
| `meta.schema_version` | `meta_schema_version` | INTEGER in `0..UINT32_MAX` |
| `meta.created_at` / `meta.updated_at` | `created_at` / `updated_at` | verbatim TEXT; an empty string stays `''` |
| `meta.status` | `session_status` | stable JSON name |
| `meta.error` | `meta_error_json` | SQL NULL = absent; `'null'` = engaged null; otherwise raw JSON |
| `system_prompt.heading_level` | `heading_level` | INTEGER; any `int` (deliberately not range-checked) |
| `extras` | `extras_json` | SQL NULL = absent; `'null'` = engaged null; `'[]'`, `'{}'` or other JSON verbatim |
| — | `kind` | `current` or `archive` |
| — | `revision`, `operation_id` | current snapshot revision; archives keep the original and replacement revisions |

**Prompt and tools**

| dataclass | table.column | encoding |
| --- | --- | --- |
| `system_prompt.sections[i].name` | `prompt_sections.name`, `ordinal = i` | one row per section; `UNIQUE(snapshot_id, name)` |
| `.title`, `.stability`, `.text` | `prompt_sections.title`, `.stability`, `.body` | verbatim; stability as its JSON name |
| `tools[i].name`, `.description` | `tool_definitions.name`, `.description`, `ordinal = i` | verbatim |
| `tools[i].argument_schema` | `tool_definitions.argument_schema_json` | verbatim JSON text |
| `tools[i].remote_type` | `tool_definitions.remote_type` | SQL NULL = absent; `''` = engaged empty string |
| `tools[i].extras` | `tool_definitions.extras_json` | NULL = absent; `'null'` = engaged null; else JSON |

**Conversation**

| dataclass | table / row | encoding |
| --- | --- | --- |
| `turns[i].retain_priority`, `.extras` | `turns`, `ordinal = i` | JSON name / nullable JSON |
| `turns[i].user_input` | `messages(slot = 'user_input', turn_id)` | exactly one row per turn |
| `.agent_loop_step[j].retain_priority` | `agent_steps`, `ordinal = j` | JSON name |
| `.agent_loop_step[j].commit_sequence` | `agent_steps.commit_sequence_u64` | 20-digit decimal TEXT |
| `.agent_loop_step[j].extras` | `agent_steps.extras_json` | nullable JSON |
| `.agent_loop_step[j].model_response` | `messages(slot = 'model_response', step_id)` | exactly one row per step |
| `.agent_loop_step[j].invoke_returns` | `agent_steps.invoke_returns_present` + `messages(slot = 'invoke_return', ordinal = k)` | flag `0` = absent, `1` = present even when empty |
| `MessageItem.type`, `.role` | `messages.type`, `.role` | JSON name / verbatim; `type` is never derived from `slot` |
| `.content[i]` | `message_contents(slot = 'body', ordinal = i)` | ordered; content is never deduplicated (D9) |
| `.reasoning`, `.action_status` | `message_contents(slot = 'reasoning' / 'action_status', ordinal = 0)` | row present = field present; unique index enforces one |
| `Content.type`, `.modality`, `.raw` | `message_contents.type`, `.modality`, `.raw` | JSON names / verbatim TEXT including embedded NUL |
| `Content.extras` | `message_contents.extras_json` | nullable JSON |
| `.invokes` | `messages.invokes_present` + `message_invokes(ordinal = i)` | flag `0` = absent, `1` = present even when empty; call ids are not unique |
| `.cost` | `message_costs` row | row present = field present; an all-zero cost differs from absent |
| `.invoke_return` | `message_invoke_returns` row | complete query + output, independent of `.invokes` |
| `MessageItem.extras` | `messages.extras_json` | nullable JSON |

**Invoke occurrences (D3)** — every occurrence is its own row; `wire_call_id`
may be empty and may repeat:

| C++ location | table | columns |
| --- | --- | --- |
| `MessageItem.invokes[i]` | `message_invokes` | `query_type`, `query_security`, `wire_call_id`, `name`, `arguments_json`, `extras_json` |
| `MessageItem.invoke_return` | `message_invoke_returns` | `query_*`, `output_*`, `extras_json` |
| `LoopProgress.pending_results[i]` | `pending_results` | `query_*`, `output_*`, `extras_json` |

**Loop progress and counters**

| dataclass | column | encoding |
| --- | --- | --- |
| `loop` | `loop_progress` row | row present = field present |
| `loop.status`, `.phase`, `.error` | `loop_progress.status`, `.phase`, `.error` | JSON names / verbatim TEXT including `''` |
| `loop.completed_exchanges` | `completed_exchanges_u64` | 20-digit decimal TEXT |
| `loop.committed_response_sequence` | `committed_response_sequence_u64` | 20-digit decimal TEXT |
| `loop.pending_results[i]` | `pending_results`, `ordinal = i` | complete batch; one row per result |
| `TokenCost.prompt`, `.generated`, `.cache_hit` | `message_costs.*_u64` | 20-digit decimal TEXT — never a JSON number or `double` |

`uint64` decoding requires exactly 20 digits and rejects non-digits or a value
above `UINT64_MAX`. Internal counters (`revision`, `ordinal`, `sequence`) stay
INTEGER with overflow checks.

### Read and write operations

- `replace_current` rewrites the current graph in one transaction: it deletes the
  current snapshot header (children cascade), reinserts it under the same
  `snapshot_id`, inserts every child row and updates `sessions.updated_at`.
  Archive snapshots are never touched. The stable snapshot id is an internal
  implementation detail; the StateStore sets `revision` and `operation_id` in
  the same transaction.
- `store_archive_snapshot` only inserts `kind = 'archive'`; the module exposes no
  update path for an archive.
- `load_current` and `decode_snapshot` rebuild in one read transaction and
  validate cardinality (one user input per turn, one model response per step),
  presence flags against child rows, snapshot ownership through foreign keys and
  JSON syntax. A damaged graph raises `SnapshotError` with `CorruptSnapshot`; it
  is never reported as an empty session.
- `extras`, `external_status` and `events` stay opaque JSON inside
  `snapshots.extras_json` (D7); they are not split into tables, and unknown keys
  round-trip verbatim.

Validation: `test_persistence_sqlite_schema` covers creation, repeated opens,
read-only inspection, version refusal, foreign/incomplete/session-mismatch
rejection and transactional migration rollback.
`test_persistence_sqlite_snapshot` covers complete-state round trips,
`UINT64_MAX`, absent/null/empty distinctions, repeated and empty call ids,
non-deduplication of content, whole-graph replacement, archive immutability,
cross-snapshot rejection and corruption detection.

## SQLite checkpoints and backend selection

New empty sessions default to SQLite at `<state>/state.sqlite3`. Explicit
`backend: json` retains the JSON adapter. Without a backend selection, an existing
JSON-only directory stays JSON and a SQLite-only directory stays SQLite. A
directory holding both stores is refused for every setting, including an explicit
backend: honoring one would abandon the other history, and no migration is
provided, so remove or migrate one store first. Legacy `format: json` selects
JSON and conflicts with explicit SQLite. Selecting the opposite backend over the
sole existing store fails, including with `restore: never`.

Manual and automatic compact freeze complete originals in the same database.
`simplex manage read_state` uses the same C++ mapper in a read-only transaction;
Hub receives decoded JSON, never database bytes. Archive queries use the separate read-only `ArchiveReader` API.

Every checkpoint replaces the current normalized graph and advances its revision
and random operation ID in one transaction. The caller can require an expected
revision; stale writes fail. No full snapshot is retained per checkpoint.
Connections enable foreign keys, WAL and FULL synchronization; database files
are created with mode 0600. A required failure latches worker storage failure,
stops admission and prevents shutdown from overwriting recovery evidence.
COMMIT/directory-sync failures are conservatively `Unknown`: the store refuses
further writes until reopened. `load_current()` returns the persisted revision
and operation ID with its graph in one read transaction, permitting comparison
with the failing operation. SQLite BUSY/FULL/IOERR, corruption, unsupported
schemas and session mismatch never trigger JSON fallback or an empty session.
A missing path is the only absent-file case; an existing empty file is rejected.

The application owns one storage thread. `loop::Options::checkpoint` supplies
awaited barriers after synchronous checkpoint events, before dispatch or result
projection. The loop's state remains frozen until the storage task completes;
read-only history/status requests may still run on the application strand.
Cancellation does not abandon a write: it joins the checkpoint and then follows
the normal safe boundary. Restore and shutdown checkpoints use the same executor.
No database transaction spans a model/tool wait. Compact archive preparation, settlement, commit and retention also run on the storage executor, for both backends.


### SQLite compact archives

T1 commits the complete original in-memory state as an immutable archive graph
and a `prepared` attempt before calling the summarizer. It records mode, run/call
identity and sequence. In the same commit it synchronizes current state to that
original, even if ordinary saves were disabled, and returns the advanced revision
for T2. Thus restart during summarization restores all original edits.
No connection or transaction remains open while the model is suspended.

T2 checks the T1 revision, replaces current state, increments revision/operation
identity and sets the attempt to `succeeded` in one transaction. Only then may the
worker replace live state and emit `compact_finished`. Failure/cancellation settles
the attempt without changing its original graph. On reopening, read-only loading
leaves evidence untouched; the first successful writer marks abandoned prepared
attempts `interrupted`. No summarization or tool execution is replayed.

Retention keeps the current attempt, supplied protected references, and prepared
recovery evidence. An interrupted archive remains protected while current state
is still at its T1 revision; a later authoritative checkpoint or compaction
resolves that recovery candidate. It then participates in the same newest-first
retention as other settled archives, with the default target of five and zero
disabling deletion. Repeated crashes do not permanently expand the protected set.
Archive rows and their owned graphs are deleted atomically; current state and
legacy filesystem artifacts are untouched. `removed_bytes` is zero for SQLite:
freed pages are reusable, not a promise of physical file shrinkage. No automatic
VACUUM, reference index or permanent JSON duplicate is added. The read-only
archive API below observes the same retained evidence.

Unknown outcomes latch all writes, including settlement and retention. T1's
operation ID is stored on its archive snapshot; T2's ID is stored on current state.
The stable archive ID correlates attempt evidence after an interrupted response.
If optional settlement or retention becomes uncertain, the worker also latches
storage failure, skips further cleanup/checkpoints/continuation and stops admission.
Its terminal `error` has `durable: false`, the storage diagnostic, and the original
compact outcome (failure, cancellation or a successfully committed replacement).
It does not retry the mutation or clear the store's uncertainty latch.

The opt-in [benchmark](benchmark/README.md) records same-host JSON/SQLite costs,
reference budgets, and the limits of the current full-graph rewrite strategy.

## SQLite archive index (schema v2)

SQLite v2 adds `archive_text_sources`, `archive_chunks`, and the external-content
`archive_chunks_fts` FTS5 table using the built-in trigram tokenizer. The source
view selects user/model message bodies, tool names and serialized arguments,
textual tool results, and pending tool evidence. Reasoning, action status, opaque
extras, system prompts, tool declarations, and binary/external-reference content
are excluded. The authoritative snapshot is unchanged and remains complete.

Chunks cover all selected text in at most 4096 UTF-8 bytes with at least
1024 bytes of overlap. Splits prefer nearby newlines. Search-only normalization
replaces NUL and invalid UTF-8 bytes with one-byte spaces, preserving byte
positions; original text is never modified. Each chunk records the archive,
source identity, conversation position, kind, source byte length and half-open
byte range. Empty sources need no chunks. There is no total-source truncation.

`begin_archive()` publishes chunks and FTS entries in the same transaction as the
original snapshot and attempt record. Insert/delete/update triggers synchronize
FTS with chunks. Retention cascades from archives to chunks and deletes indexed
text in that same transaction. Index errors roll back archive preparation;
ordinary current-state checkpoints do not index the current conversation.
All attempt outcomes remain searchable evidence, including prepared, failed,
cancelled and interrupted attempts. An attempt outcome is not a tool outcome.

New stores use schema v2 with the unchanged v1 state codec. Existing v1 stores
remain readable by worker restoration and read-only manager inspection. Their
next normal writer operation atomically upgrades the schema and backfills all
retained archives, processing one source at a time. Upgrade failure leaves the
v1 schema and authoritative state intact. Older workers that only understand v1
cannot open the upgraded store. Reads never upgrade or repair storage.

## Archive query API

`<persistence/archive_query.hpp>` exposes a session-scoped `ArchiveReader`:

```cpp
auto reader = persistence::make_sqlite_archive_reader(session_id, state_directory);
auto archives = reader->list(20); // newest first; last sequence is the next cursor
persistence::ArchiveSearchRequest request;
request.text = "configuration path";
auto matches = reader->search(request);
if (!matches.hits.empty()) {
    const auto& hit = matches.hits.front();
    auto excerpt = reader->read(hit.archive.archive_id, hit.source_id, hit.start_byte);
    // Continue at excerpt.end_byte while excerpt.has_more is true.
}
```

These calls are synchronous and belong on a storage executor. Construction does
no I/O. Each call uses a separate read-only connection/transaction and validates
the expected session. Concurrent calls are allowed; a query never shares the
writer's connection, creates a missing database, upgrades v1, or repairs indexes.
A v1 store reports `IndexUnavailable` until its owner's next normal write upgrades
it. A missing or corrupted store is an error, not an empty result. JSON has no
archive-query implementation or implicit SQLite fallback.

Search text is a **literal substring**, not a Boolean/FTS expression or semantic
query. It must be valid UTF-8 without NUL, contain a non-whitespace character,
and fit in 256 bytes. Three or more Unicode scalars use FTS5 trigram matching
(case-insensitive); shorter queries use an exact case-sensitive scan. Quoted
operators and punctuation remain literal. A query has at most 256 Unicode
scalars; the 1024-byte overlap covers case-equivalent source scalars even when
their UTF-8 byte lengths differ. Search supports archive and text-kind filters, 1..100 hits (default 10),
and 32..4096 preview bytes (default 512). Results use BM25 order, then newest
archive and source position. Exact repeated chunks of the same kind and
within-source overlapping hits are collapsed; this is not semantic deduplication
of entire conversations. Different overlapping chunks in different archives can
still produce distinct hits. `has_more` means another distinct hit was found;
there is no expensive total count or cross-request relevance cursor.

Hits include archive identity/outcome/time, source identity, optional zero-based
turn/step positions, the source's half-open byte range, and a bounded UTF-8
snippet with an explicit truncation flag. Previews use marker-safe FTS
highlights to locate source matches, then select UTF-8-safe context within the
byte allowance. A complete match is retained whenever it fits, even if case
folding changes its spelling or byte length. No ellipsis consumes preview space;
the truncation flag reports omissions, including an oversized matched span. Pending evidence has no claimed turn
or step. `ToolCall` sources project the name, a newline, and serialized arguments;
other sources project their original text. Reading accepts 4..65536 bytes
(default 16384), preserves NUL and invalid bytes, and does not split valid UTF-8
scalars. An offset inside a valid scalar is rejected; resume with the returned
`end_byte`. Paging can retrieve the entire source without a total-content cap.

A source id is meaningful only with its archive id. Retention can run between
search and read: a removed archive returns `NotFound` and cannot alias a later
SQLite row. Each call sees one consistent snapshot, but calls do not pin archive
lifetimes. The existing `memory_retention.max_archives` target (default 5) still
bounds the searchable history; no permanent memory copy is created.

Every operation has a cooperative 1..30000 ms budget (default 1000 ms) and an
optional `std::stop_token`. SQLite VM work and application loops observe it.
Transient SQLite lock contention shares the same deadline.
Cancellation/timeouts return explicit errors, never partial results or a fake
empty match set. Filesystem stalls are not hard realtime bounded. Query failure
does not latch an authoritative storage failure or prevent later checkpoints.

This API currently has no Agent tool, Hub protocol, manager command, or prompt
integration. It establishes the storage/query layer for those future consumers.
