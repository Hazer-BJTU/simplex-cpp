# SQLite schema v1 and entity mapping

**Status:** accepted · #187 PR-C · owner confirmation: @Hazer-BJTU, 2026-10-18
(the PR-C design review recorded below; decisions D1–D10 were confirmed
explicitly before implementation started)

This page is the owner-confirmed schema decision for the normalized SQLite
backend. It supersedes the candidate tables in #187 §3.2, which were explicitly
discussion input. PR-C implements the executable DDL, the row↔dataclass mapping
and the scalar codecs; wiring checkpoints, revisions and compaction into the
worker remains PR-D/PR-E work.

## Context

PR-A introduced the backend-neutral `persistence::StateStore` contract and PR-B
replaced lossy Markdown archives with complete JSON state archives. The JSON
backend is authoritative today. #187 requires a normalized SQLite backend for
future retrieval, but forbids implementing query interfaces, search indexes or
reference management in this phase. The owner therefore had to decide the
physical model first: entity boundaries, identities, ordering, open-JSON
handling, and how archives reuse the model.

## Owner-confirmed decisions

**D1 — Archives are normalized snapshots; memory archives are deferred.**
A *snapshot archive* is compact evidence: the complete original
`AgentInputState` stored as a normalized snapshot with `kind = 'archive'`.
A *memory archive* — a future retrieval/memory-oriented archive — is not
implemented in this phase. Both will be distinguished by
`archives.archive_kind`; only `'snapshot'` is writable now. `archives` will
change again when memory archives are designed (D4).

**D2 — No cross-snapshot stable entity IDs.** Entities are identified by
database row IDs local to one snapshot, and order is carried by `ordinal`.
Public dataclasses gain no identity fields.

**D3 — Every occurrence of an `InvokeQuery`/`InvokeReturn` is stored
independently.** The same logical call appears in three places that represent
*different facts*: the model's original proposal (`MessageItem.invokes`), the
call as confirmed/executed and returned (`InvokeReturn.query`), and the
unprojected result batch (`LoopProgress.pending_results`). These are not
duplicates: argument-default completion may rewrite a call in place, and the
wire call id may be empty or repeated. The schema therefore uses
occurrence-inline tables (`message_invokes`, `message_invoke_returns`,
`pending_results`) with no shared query identity and no deduplication.

**D4 — Create `archives` now, knowing it will change.** PR-E needs attempt
metadata; the table is created in v1, but its final shape follows the memory
archive design and is out of scope for this issue.

**D5 — No retrieval indexes or version tables yet.** FTS5, chunking,
embeddings, references, pins and `search_schema_version` wait for the retrieval
design. They must arrive as rebuildable derived data, never as a replacement for
the authoritative rows.

**D6 — Current snapshots are rewritten as a whole graph.** One transaction
deletes the current snapshot's child rows and inserts the new graph. A
row-level diff is a possible later optimization, not a schema concern.

**D7 — `extras` is stored as one opaque JSON string.** `external_status`,
`events` and every plugin-defined field are internal host state, not
model-facing content, and their protocols are unknown to storage. They are not
split into tables; exact round-trip is the requirement.

**D8 — `uint64` values use 20-digit decimal TEXT.** Token costs and sequence
counters must survive `UINT64_MAX` and must not pass through a JavaScript
number. Internal counters (`revision`, `ordinal`, `sequence`) use
overflow-checked INTEGER.

**D9 — Content is never deduplicated.** Each content occurrence is its own row.

**D10 — One database per session** at `<state>/state.sqlite3`.

## Storage layout

```text
<persistence.directory>/<persistence.state>/state.sqlite3
```

One database belongs to exactly one session (`sessions` holds one row). A store
has one `current` snapshot and any number of immutable `archive` snapshots,
selected through a partial unique index rather than a cyclic foreign key.

## Entity diagram

```text
sessions 1 ──< snapshots ──< prompt_sections
                    │        ├── tool_definitions
                    │        ├── turns ──< agent_steps
                    │        │        └──< messages ──< message_contents
                    │        │                      ├──< message_invokes
                    │        │                      ├──  message_costs
                    │        │                      └──  message_invoke_returns
                    │        ├── loop_progress
                    │        ├── pending_results
                    │        └── extras_json (opaque JSON)
                    └──< archives ──1 original snapshot (kind='archive')
```

Every child row carries `snapshot_id`, and composite foreign keys make a
cross-snapshot reference impossible. Messages are leaves that point at their
owner (`turn_id` for the user input, `step_id` for model responses and tool
results); invariants that a unique index cannot express — exactly one user
input per turn, exactly one model response per step — are validated by the
codec before a commit.

## Encoding rules

The column-by-column mapping lives in
[the persistence module documentation](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/persistence/README.md#sqlite-schema-v1).
The rules every mapping follows:

- **Absent vs empty vs null.** A missing optional field is a missing row or a
  SQL NULL; an empty string is `TEXT ''`; an empty array keeps its presence flag
  (`invokes_present`, `invoke_returns_present`); `optional<json>` distinguishes
  SQL NULL from the JSON text `null`. Nothing collapses these states.
- **Open JSON.** `extras`, `arguments`, `argument_schema`, diagnostic and
  content-extras payloads are stored as JSON text and decoded by the dataclass
  codec. Database constraints never encode a stricter rule than the codec; for
  example `heading_level` is not range-checked, because the dataclass accepts
  any integer and validates only when rendering.
- **Enums** are stored under their stable JSON names; reading an unknown name
  follows the dataclass decoder instead of inventing a database default.
- **Ordering** is always explicit in an `ordinal` column; no row order is
  implied by insertion order.

## Archive model

An archive is `snapshots.kind = 'archive'` plus the same normalized children,
referenced by `archives.original_snapshot_id`. Archive snapshots are immutable:
the codec inserts them and never updates them; retention deletes the `archives`
row and then the snapshot, whose children cascade. `archive_kind` distinguishes
snapshot archives from the deferred memory archives. Retention policy itself is
unchanged from the JSON backend and is implemented in PR-E.

## Scope boundaries

- **PR-C (this change):** executable schema v1, migrations, row mapping, scalar
  codecs, opaque-extras rules, minimal read-only/insert/update operations and
  their tests. No `StateStore` implementation, no checkpoint scheduling, no
  worker integration.
- **PR-D:** SQLite `StateStore`, revision/operation identity, transactional
  checkpoints, read-only state loading and the storage executor.
- **PR-E:** compact archive attempts (T1/T2), archive rows, retention.
- **PR-F:** migration/export/rollback tools, including how legacy
  `legacy_readable` evidence is recorded.
- **PR-G/PR-H:** Hub/Panel integration and the default backend.

## Deferred questions

- The memory archive representation and retrieval design (full-text search,
  chunking, embeddings, reference tracking, `search_schema_version`).
- The final `archives` shape (D4).
- Row-level diff optimization for current snapshots (D6) and compaction
  benchmarks.

## Consequences

- Retrieval can later start from normalized history and archives without a
  schema rewrite, and archives need no decoding pass.
- A compact attempt copies a full snapshot graph. That matches what the JSON
  backend already writes to disk; large-history benchmarks remain PR-D/E work.
- `extras` cannot be filtered by source or event name in SQL. A future query
  feature must parse the JSON or add a rebuildable projection.
- Every occurrence of a tool call is a separate row, so "all tool calls in this
  session" is a view over three tables rather than one table.

## Validation

`test_persistence_sqlite_schema` covers creation from an empty database,
repeated opens, future-version refusal without modification, foreign-database
and session-mismatch rejection, read-only inspection and transactional
migration rollback. `test_persistence_sqlite_snapshot` covers complete-state
round trips, `UINT64_MAX`, absent/empty distinctions, repeated and empty call
ids, opaque extras, edit/delete/reorder, archive equality and foreign-key
rejection. See the PR-C description for the exact commands and results.
