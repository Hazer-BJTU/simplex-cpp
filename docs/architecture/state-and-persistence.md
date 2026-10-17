# State and persistence

The authoritative conversation is `model_io::AgentInputState`. It contains the
prompt, tools, turns, provider metadata, extras, and optional loop progress.
Live registries, credentials, subscriptions, child handles, and event queues are
runtime dependencies and are not restored from this snapshot.

## Storage ownership

With an opt-in **Managed worker** launch profile, Hub and target ownership are
separate:

```text
Hub: <dataDir>/sessions/<session-id>/
  config/                 source/launch snapshots and library provenance
  allocation.json         private target identity and cleanup intent
  events.jsonl, logs/, plan.json

Target: <management.root>/<allocation-id>/
  config/config.yaml      generated managed reference
  state/state.json, state/readable.md
  memory/, session.lock
```

Hub invokes `simplex manage` through the selected local/SSH/Docker transport for
configuration preparation, snapshot inspection and allocation removal. State is
returned as complete JSON; Hub neither opens target files nor maps databases.
See [managed sessions](../reference/management.md#opt-in-managed-sessions) for
configuration, timeout/retry policy and remote lifecycle requirements.

Legacy launch profiles and existing sessions retain this original layout:

```text
<dataDir>/sessions/<session-id>/
  config/config.yaml      worker startup configuration
  config/launch.jsonc     launch configuration snapshot
  state/state.json        authoritative worker state
  state/readable.md       optional human-readable export
  memory/<archive>/state.md
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
single-writer and confined to the worker strand. `core` asks for a checkpoint, a
reserved archive, a compaction commit or a retention pass; the storage format
stays inside the backend. JSON is the only backend in this phase and keeps the
layout below unchanged. The boundary, its ordering rules and the follow-up work
are recorded in [the persistence backend contract](persistence-backends.md).

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
exchange, and never rewritten afterwards. A committed replacement settles its own
attempt, so success is never a separate step that could fail after the commit;
failure, cancellation and interruption are recorded explicitly. Retention policy
and deletion targets are unchanged.

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

## Human-readable exports and compaction

Readable Markdown is write-only. Long JSON previews are clipped and binary
payloads are omitted to keep exports navigable. Use JSON for restoration.

Compaction first preflights fixed overhead against a 64 KiB replacement byte
budget, then archives the old state and runs an isolated summarization exchange.
It validates the summary, then durably publishes the replacement before announcing
success. The summary replaces prior injected memory and removes user turns.
The memory prompt points to archive paths, which are meaningful on the worker's
filesystem. Failed or cancelled compaction preserves the original conversation.

After each archived attempt, including failure/cancellation,
`memory_retention.max_archives` targets five recognized archives by default.
Zero disables cleanup. The current attempt and archives explicitly referenced
by absolute paths in retained state are protected, even above the target.
Required replacement write failures protect both old and replacement references:
rename may succeed before a directory-sync error leaves crash durability uncertain.
Other recognized archives are retained newest first; unknown files, symlinks,
and incomplete directories are not removed. Cleanup errors preserve the primary
outcome and are reported through optional diagnostics.
This is a retention target, not a disk quota.

## Hub history is a projection

The Hub's event transcript is an execution record. Its in-memory replay buffer
starts afresh after Hub restart; JSONL is not automatically replayed into it.
A connected worker can return bounded display-history pages to restore the
panel. Neither these pages nor the Hub's transcript replaces `state.json`.

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
