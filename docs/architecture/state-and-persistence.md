# State and persistence

The authoritative conversation is `model_io::AgentInputState`. It contains the
prompt, tools, turns, provider metadata, extras, and optional loop progress.
Live registries, credentials, subscriptions, child handles, and event queues are
runtime dependencies and are not restored from this snapshot.

## Storage ownership

New local launch templates and fresh native API sessions with SQLite persistence
use target-side management. Explicit **Managed worker** profiles support other
targets. Hub and target ownership are separate:

```text
Hub: <dataDir>/sessions/<session-id>/
  config/                 source/launch snapshots and library provenance
  allocation.json         private target identity and cleanup intent
  events.jsonl, logs/, plan.json

Target: <management.root>/<allocation-id>/
  config/config.yaml      generated managed reference
  state/state.sqlite3     normalized current state and compact archives
  memory/, session.lock
```

Hub invokes `simplex manage` through the selected local/SSH/Docker transport for
configuration preparation, snapshot inspection and allocation removal. State is
returned as complete JSON; Hub neither opens target files nor maps databases.
Default local allocations use `<dataDir>-workers` as their management root.
Existing source files and legacy launch profiles keep their original binding.
See [managed sessions](../reference/management.md#opt-in-managed-sessions) for
configuration, timeout/retry policy and remote lifecycle requirements.

Legacy launch profiles and existing sessions retain this original layout:

```text
<dataDir>/sessions/<session-id>/
  config/config.yaml      worker startup configuration
  config/launch.jsonc     launch configuration snapshot
  state/state.json        authoritative worker state
  memory/<archive>/       compact evidence: state.json, manifest.json
  events.jsonl            Hub-observed events
  logs/worker.log          locally captured process output
  plan.json               Hub-owned remote-tool plan
  session.lock            worker persistence ownership
```

`persistence.directory` is the direct root; the worker adds no session suffix.
`state` and `memory` are configurable relative children. The Hub stores its own
registry and reusable configurations outside individual session roots.

## Backend boundary

The worker reaches authoritative state only through `persistence::StateStore`
(`persistence/include/persistence/store.hpp`). A store is session-scoped,
single-writer with serialized operations. Current-state I/O runs on a dedicated
storage executor and is awaited by the worker. `core` asks for a checkpoint, a
reserved archive, a compaction commit or a retention pass; the storage format
stays inside the backend. New empty sessions default to SQLite. Existing JSON-only
storage remains JSON when backend selection is omitted. Explicit choices never
silently migrate or abandon existing history. See [SQLite schema v1](sqlite-schema-v1.md)
and [the persistence backend contract](persistence-backends.md).

Every store operation is synchronous. A checkpoint or compaction commit reports
one of three outcomes: `NotCommitted` (the previous state is still
authoritative), `Committed`, or `Unknown` (publication may have happened with
uncertain crash durability). The worker stops writing on a failure and keeps its
recovery evidence instead of retrying blindly, and it verifies an `Unknown`
outcome after reopening. Codec and filesystem failures surface as a store error;
only a programming error stays outside that contract. A backend that exposes a
revision must refuse a stale write; the JSON backend exposes none and rejects an
expectation instead of ignoring it.

Compact archives are reserved during preflight, frozen before the summary
exchange, and never rewritten afterwards. For JSON, each archive is a directory containing
the complete original `state.json` and a `manifest.json` publication marker that
is written last; a directory without a complete, self-consistent manifest is
incomplete evidence and never restorable. A committed replacement settles its own
attempt, so success is never a separate step that could fail after the commit;
failure, cancellation and interruption are implicit in the frozen archive plus the
unchanged authoritative snapshot. Retention policy and deletion targets are
unchanged.

`persistence.backend: sqlite` stores normalized current state in
`<state>/state.sqlite3`, with transactional revisions and operation IDs. It
supports checkpoints, restoration, compact and auto compact. T1 freezes the full
original state and a prepared attempt, synchronizing current state even when
ordinary saves are disabled; T2 atomically replaces current state and
marks that attempt succeeded. No transaction spans the model request. Abandoned
prepared attempts become interrupted on the next writer operation, while
read-only restoration never rewrites evidence.
Use a new session directory; backend selection does not migrate existing JSON.
Migration is deferred. Managed Hub inspection uses the C++ mapper and new empty sessions select SQLite.

## Checkpoints and restoration

Before dispatching tools, persistence records the tools phase. After dispatch,
it records the complete pending results before projecting them. These recovery
checkpoints are independent of optional step/run save switches when persistence
is enabled. This ordering preserves evidence of ambiguous external effects.

JSON saves use atomic file replacement. A persistence error can stop further
admission and suppress later saves to avoid overwriting recovery evidence.
Atomic publication cannot make external tool effects and disk state one
transaction. A crash can still leave an ambiguous tool outcome.

`restore: if_present` loads an existing snapshot; `never` starts fresh state.
Do not use `never` as a backup strategy: later saves use the same destination.
Restoration does not start another loop automatically or recreate old process
handles. Runtime prompt sections for skills, environment, and signature are
refreshed; stored conversation memory remains.

## Markdown exports and compaction

Readable Markdown is deprecated since #187 PR-B. The worker no longer writes
`readable.md`, and the explicit `load::StateFormat::Readable` exporter is a
compatibility API that does not participate in saves, compaction or recovery.
Complete JSON state is the authoritative artifact; use it for inspection and
restoration. `persistence.readable` is still accepted so existing configurations
start unchanged, but it only produces one startup diagnostic.

Compaction first preflights fixed overhead against a 64 KiB replacement byte
budget, then archives the complete old state as JSON and runs an isolated
summarization exchange. It validates the summary, then durably publishes the
replacement before announcing success. The summary replaces prior injected memory
and removes user turns. The memory prompt names the archive directory and the
latest complete-state file, both of which are meaningful on the worker's
filesystem. Failed or cancelled compaction preserves the original conversation
and the frozen archive.

After each archived attempt, including failure/cancellation,
`memory_retention.max_archives` targets five complete archives by default.
For JSON archives, the filesystem rules below apply. SQLite retention uses the
same count target and keeps the current attempt plus unresolved prepared or
interrupted recovery evidence; it never deletes unknown filesystem artifacts.
Zero disables cleanup. The current attempt and archives explicitly referenced by
absolute paths in retained state are protected, even above the target.
Required replacement write failures protect both old and replacement references:
rename may succeed before a directory-sync error leaves crash durability uncertain.
Other complete JSON archives are retained newest first. Legacy `state.md`
archives, unknown files, symlinks, extra files and incomplete directories are
never candidates: legacy Markdown cannot be reconstructed, so it stays as
evidence. Cleanup errors preserve the primary outcome and are reported through
optional diagnostics. This is a retention target, not a disk quota.

## Hub history is a projection

The Hub's event transcript is an execution record. Its in-memory replay buffer
starts afresh after Hub restart; JSONL is not automatically replayed into it.
A connected worker can return bounded display-history pages to restore the
panel. Neither these pages nor the Hub's transcript replaces the authoritative store.

## Headless delegation

The Hub supports clean-fork/send/receive remote routes for directly owned headless
workers. Each child uses a flat `<dataDir>/subagents/<generated-id>/` Hub bookkeeping root, an
independent operator-controlled ask/deny/approve policy, and a bounded primary
conversation projection. It retains no event transcript or reasoning/tool history.
Parent process shutdown/crash cascades; socket disconnect alone preserves the
family. Managed children store worker state separately at
`management.root/<allocation-id>/`; target manager removal must succeed before
Hub deletes local ownership. Legacy children retain colocated storage.
Confirmed child shutdown deletes its persistence. Unknown launch ownership
retains data and quota; bounded retries and an authenticated, lifecycle-bound operator
recovery API prevent indefinite retry churn without assuming missing records mean
termination. Clean-fork shares any explicit external workspace and is not a sandbox.
The optional C++ `hub_remote_call` toolset exposes fork/send/receive operations.
See the [complete subagent contract](../hub/subagents.md) for configuration snapshots,
launch support, request deduplication, recovery and cleanup limits.

## Selection, inspection, and backup

New templates explicitly select SQLite. Saved Hub worker configurations are not
rewritten on upgrade. Legacy `format: json` selects JSON; remove this alias only
when deliberately creating a separate empty SQLite session. With neither setting,
JSON-only and SQLite-only stores are detected, empty storage selects SQLite, and
both stores require an explicit choice. `restore: never` still validates the
selected store and its session identity before starting fresh. Persistence disabled
creates no database. Clean-fork copies startup policy, never current state, WAL,
or archives; each child owns an independent directory and store. An omitted
backend remains omitted in the captured policy, so an empty child selects SQLite;
explicit JSON or the legacy format alias keeps the child on JSON.

`simplex manage read_state` is a consistent, read-only current-state projection.
It uses the saved local/SSH/retained-Docker launcher; Hub must not open a target
file or fall back to a local copy. JSON results remain complete on the management
pipe. Inspector rendering is a preview, not a database download. No archive
browsing, migration, or cross-backend export command is provided in this release.

Use a local filesystem supported by SQLite WAL. For a consistent backup, stop the
worker, ensure all manager readers have finished, then copy the whole session
directory including any WAL/SHM files. Never copy only a live `state.sqlite3`.
Keep JSON sessions on JSON until explicit migration tooling is available.
