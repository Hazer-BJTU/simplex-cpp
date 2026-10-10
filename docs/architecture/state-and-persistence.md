# State and persistence

The authoritative conversation is `model_io::AgentInputState`. It contains the
prompt, tools, turns, provider metadata, extras, and optional loop progress.
Live registries, credentials, subscriptions, child handles, and event queues are
runtime dependencies and are not restored from this snapshot.

## Storage ownership

For a Hub-managed worker:

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
workers. Each child uses a flat `<dataDir>/subagents/<generated-id>/` root, an
independent operator-controlled ask/deny/approve policy, and a bounded primary
conversation projection. It retains no event transcript or reasoning/tool history.
Parent process shutdown/crash cascades; socket disconnect alone preserves the
family. Confirmed child shutdown deletes its persistence. Unknown launch ownership
retains data and quota; bounded retries and an authenticated, lifecycle-bound operator
recovery API prevent indefinite retry churn without assuming missing records mean
termination. Clean-fork shares any explicit external workspace and is not a sandbox.
The optional C++ `hub_remote_call` toolset exposes fork/send/receive operations.
See the [complete subagent contract](../hub/subagents.md) for configuration snapshots,
launch support, request deduplication, recovery and cleanup limits.
