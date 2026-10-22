# Worker storage management

`simplex manage` runs a target-local management program installed beside the
worker. It prepares and inspects managed allocations and removes them after
worker shutdown. The manager supports JSON and normalized SQLite storage.
Hub launch profiles can opt new ordinary sessions into this interface using
`management`. Their headless descendants use the same manager boundary. Existing
profiles remain on their explicitly supported legacy layout.

## Invocation and trust

```sh
simplex manage --help
simplex manage --request BASE64
```

The argument is standard padded Base64 encoding of one UTF-8 JSON object.
There is no stdin request protocol. stdout contains one complete JSON response;
errors also produce a response and a nonzero exit status. Help/version output is
plain text. Diagnostics must not be mixed into stdout. Success requires both a
successful process exit and a matching successful response.

The configured peer is trusted. Management results are not clipped and have no
byte limit; callers can collect and cache the complete result in memory. Use
asynchronous process pipes, draining stdout and stderr together, without an
`exec`/`execFile` default buffer cap. This contract does not change ordinary
worker WebSocket display/preview limits.

Base64 is not encryption. Encoded arguments may be visible to privileged local
process inspection. Avoid logging them. OS/SSH argv limits still apply; oversized
requests fail rather than being truncated or silently sent using another channel.

## Request and response

All requests include these fields:

```json
{
  "version": 1,
  "request_id": "operation-1",
  "operation": "inspect",
  "allocation_id": "allocation-1",
  "directory": "/srv/simplex/allocations/allocation-1"
}
```

Identifiers are 1–128 ASCII letters, digits, underscores or hyphens. `directory`
is a normalized absolute path **on the target**. Symlinks in managed path
components are refused. No arbitrary file-read or SQL operation is exposed.
Use a target root distinct from the Hub's bookkeeping directory.

A successful response has `version`, `request_id`, `ok: true`, and `result`.
An unsuccessful response has `ok: false` and `error: {code, message}`; an invalid
request may have a null request ID. Atomic publication failures may additionally
report `error.published: true`. Inspect/reconcile after a lost response or an
uncertain publication; transport failure does not imply no change occurred.

| Operation | Additional fields | Result / behavior |
| --- | --- | --- |
| `prepare` | `configuration`: raw YAML string on first creation; optional `refresh` | Creates a new allocation. Repeated identical requests preserve it. Refuses an existing unmanaged directory. |
| `prepare` (refresh) | `refresh`, `expected_revision` when changing existing overrides | Updates only connection overrides; preserves source YAML verbatim and advances revision. Identical override retries are harmless. |
| `inspect` | None | Returns status, revision, config path, source/effective configuration and captured startup data when ready. Does not create missing allocations. |
| `capture_startup` | `lifecycle_id`, `expected_revision` | Captures the effective configuration for that lifecycle. Refuses a changed capture under the same lifecycle ID. |
| `clean_fork` | `source: {allocation_id, directory, lifecycle_id, revision}`; optional `refresh` | Creates the requested child allocation from a captured parent startup on the same target. Never copies runtime state or archives. |
| `read_state` | None | Returns `{backend: "json" \| "sqlite", state, readable}`; SQLite also includes current revision and operation ID when present. Missing state/readable exports are null; corruption is an error. Complete state, including compatible unknown JSON fields, is retained. |
| `remove` | None | Requires exclusive allocation ownership and no legacy worker lock. Deletes the target root and retires the allocation; repeat removal is safe. |

`refresh` is an object with optional `client_endpoint`, `confirmation_endpoint`,
and `tools_endpoint` string fields. It is a **complete replacement** of the
previous override set. Confirmation/tool overrides only apply if the corresponding
section is enabled in the source YAML; they never re-enable an omitted toolset.
Provider credentials, unknown settings, and source comments remain unchanged.
The persistence directory is always the target allocation root. Storage `state`
and `memory` values must remain relative child paths without parent traversal.

Inspect responses contain configuration secrets. They are management data, not
public Panel session descriptions. `capture_startup` does not launch a process;
the launcher orders refresh, capture and launch consistently.

## Local example

Choose a fresh target path. This example creates an allocation but does not run
a model or contact an endpoint:

```sh
request=$(python3 - <<'PY'
import base64, json
from pathlib import Path
request = {
    "version": 1,
    "request_id": "prepare-demo",
    "operation": "prepare",
    "allocation_id": "demo-1",
    "directory": str(Path.home() / ".simplex-worker" / "allocations" / "demo-1"),
    "configuration": Path("worker.yaml").read_text(),
    "refresh": {"client_endpoint": "ws://127.0.0.1:8765/events"}
}
print(base64.b64encode(json.dumps(request).encode()).decode())
PY
)
simplex manage --request "$request"
```

Use `result.config_path` with `simplex run --config PATH --session SESSION_ID`.
The source YAML still needs valid provider, prompt and other worker settings;
management preparation does not load plugins or validate remote availability.
`allocation_id` is storage identity, distinct from the worker session ID.

## Target layout and ownership

```text
/srv/simplex/allocations/
├── .simplex-manager/
│   ├── demo-1.lock             # stable coordination inode; never delete live
│   └── demo-1.json             # source YAML, overrides, startup and status
└── demo-1/
    ├── config/config.yaml     # immutable managed reference, not source YAML
    ├── session.lock           # worker's exclusive writer lock
    ├── state/state.json       # worker-owned state
    └── memory/                # worker-owned archives
```

Allocation mutations coordinate on ancestor directory inodes before checking
for nested allocations. A concurrent parent/child preparation cannot both claim
the same hierarchy; contention returns `busy` and may be retried. This requires
readable ancestor directories and local POSIX directory-lock support.

The worker resolves the managed reference through the manager library and keeps
a shared allocation lease for its lifetime. Management reads may coexist with
it, while preparation, refresh, capture and removal require exclusive ownership.
The worker retains its existing exclusive writer lock as well. Do not pass a
managed reference to an older worker that does not support this contract.

Removal leaves a small identity tombstone and the stable sibling lock; it erases
source configuration and startup data from the record. Retired target paths
cannot be reused. Use a fresh directory/identity for a replacement. Existing
unmanaged roots are never adopted automatically; existing ordinary configuration
files and Hub sessions remain supported on their original paths.

SQLite inspection reads through the C++ mapper in one read-only transaction and
returns the same decoded JSON state shape. It never creates a missing database,
migrates a schema, or falls back to stale JSON after a database error. Schema,
corruption and transport failures remain explicit. Hub has no SQLite dependency.
The management pipe remains uncapped; the Inspector displays a bounded preview.
The database path is not a downloadable JSON artifact.

## Hub launcher transport boundary

The Hub's internal `ManagedLauncher` adapter provides the next integration layer.
It binds `simplex run` and `simplex manage` to one explicitly configured execution
target and installation. New local templates enable it by default, as do fresh
native API sessions with SQLite persistence and no existing source configuration.
API launches use the `simplex` router beside `worker.bin`; the local template uses
`management.executable` (default `simplex` on PATH). Their allocation root is
`<dataDir>-workers`. The **Managed worker** template supports custom target policy.
Existing profiles and sessions retain their saved launch path. Managed children
inherit the target binding and use independent allocations.

The adapter supports these invocation shapes:

| Target | Invocation | Lifecycle prerequisite |
| --- | --- | --- |
| Local | `simplex manage --request BASE64` | Installed `simplex` supports management. |
| SSH | `ssh [connection options] -T -- HOST 'simplex' 'manage' '--request' 'BASE64'` | Target uses a POSIX command shell; worker ownership must be verified independently of SSH exit. |
| Docker | `docker [context options] exec CONTAINER simplex manage --request BASE64` | Container already exists, is running, provides the configured installation and remains available through worker shutdown and allocation removal. |

SSH quotes every remote argument, including executable paths containing spaces or
apostrophes. Docker uses argument arrays with neither a shell nor a PTY. There is
no inference from arbitrary command templates, implicit container creation, or
shared-directory fallback. Hub never creates, starts, stops or removes a
container.

A Docker target runs read-only startup checks before the first target mutation:
the container must exist and be running, `docker exec CONTAINER EXE --version`
must succeed, and the advertised channel address must be usable from the
container's network namespace. A loopback address is accepted only when the
container shares the host network; a `host.docker.internal` name requires a host
mapping. `management.target.preflight` adjusts the checks, their deadline and the
inspected channels. A failed check refuses the start; a new session stays unbound, so
no allocation record is written and both configuration files remain editable.

The transport's `cwd` and environment describe the Hub-side process. Allocation
paths and the `simplex` executable describe the target. For example, Hub records
may live in `/home/operator/.simplex-hub`, while the target reference is
`/srv/simplex/allocations/allocation-1`; the latter need not exist on the Hub.
The same transport context and executable are used for run and manage. Target
settings are privately copied when constructing the adapter to prevent mutable
configuration from redirecting later calls in one lifecycle.

The transport uses asynchronous `spawn`, reads both pipes concurrently and waits
for process closure before parsing the complete UTF-8 JSON response. Neither pipe
has a receive-size budget. Nonzero exit cannot be masked by a successful response;
a successful exit with a malformed, truncated or incorrectly correlated response
is also an error. Manager-reported failures retain their stable code and optional
`published` flag. Transport/protocol failures carry an **unknown outcome** and do
not trigger automatic retries. Callers must inspect/reconcile the same allocation
identity before deciding whether a mutation can be repeated.

The low-level transport accepts an abort signal. Session integration sets a
per-command `management.timeoutMs` deadline (30,000 ms by default). A timeout
kills only the Hub-side transport, reports an unknown outcome, and retains
the allocation reference for reconciliation. It is not a receive-size limit. This adapter does not equate termination of SSH or Docker
CLI processes with remote worker termination. Captured stderr and manager error
details are private diagnostics: they can contain secrets and must not be placed
in Panel events or ordinary logs. Public error messages omit argv and raw peer
output.


## Opt-in managed sessions

For a custom root or target beyond the default local policy, create a launch
configuration using **Managed worker**, edit it, and select it
with a worker configuration when creating a **new** session. Both the installed
worker and `simplex manage` must support the allocation contract above. This
feature is not compatible with older released workers that lack the manager.

The template defaults to local execution, with allocation roots under
`~/.simplex-worker/allocations` (expanded to an absolute path by the Hub). Each
session receives a random allocation ID, unrelated to the reusable display
session ID. A retired allocation path is never reused.

A minimal launch profile is:

```json
{
  "management": {
    "target": { "kind": "local" },
    "executable": "/opt/simplex/bin/simplex",
    "root": "/srv/simplex/allocations",
    "cwd": "/home/operator",
    "env": {},
    "timeoutMs": 30000
  },
  "worker": { "threads": 2 },
  "endpoints": {}
}
```

The single `management.executable` selects both `simplex run` and `simplex manage`.
A managed profile may omit `launcher` and `worker.bin`. For existing profiles,
when `management` is present, its executable/target replaces the legacy
`launcher.command`, `launcher.args`, `launcher.cwd`, and `worker.bin` invocation.
`worker.threads`, `worker.args`, connection-host settings, endpoint overrides and
stop-policy settings remain applicable. `pidFile` is not supported. The top-level
launch `env` overlays `management.env`; both apply to the Hub-side transport,
not to the remote worker environment. For local execution they also reach the
worker; for SSH/Docker, provision worker credentials in that target environment.

`management.cwd` must be an absolute existing **Hub-local** directory. `root`
and `executable` refer to the **target**. Local roots must neither contain nor
be contained by Hub `dataDir`; existing symlink aliases are checked too. Known
config-relative worker paths remain relative to the target's generated config
directory; Hub does not resolve them into its local filesystem. Configure
absolute target paths for existing workspace/assets when appropriate.

### Remote targets

For SSH replace only the target section, and point `executable`/`root` at the
remote installation/storage:

```json
{ "kind": "ssh", "command": "ssh", "args": ["-p", "2222"], "host": "worker@example.test" }
```

For Docker use an **existing retained container**:

```json
{
  "kind": "docker",
  "command": "docker",
  "args": ["--context", "my-target"],
  "container": "simplex-target",
  "preflight": { "container": true, "executable": true,
                 "endpoint": { "enabled": true, "channels": ["events"] } }
}
```

The operator creates and keeps that container running independently of the
worker process (for example, an image with `simplex` installed and an idle init
process). Hub runs both commands using `docker exec`, with no bind mount needed.
The container must remain available after worker exit until deletion succeeds.
Hub does not create, stop or remove this container; its lifecycle must not make
the worker PID 1 with automatic container removal. Configure advertised Hub
endpoints reachable from the target as with other remote launch profiles.
[Deploy a Docker worker](../deployment/docker-worker.md) covers image
preparation, network modes, the allocation root and troubleshooting.

Remote stop sends the normal shutdown signal. A successful idle lease probe is
only a present-time observation: an SSH/Docker command may already be dispatched
but paused before lease acquisition. Stop therefore also requires durable
completion evidence for that exact dispatch. For a remote foreground command,
only an observed successful transport exit establishes completion; transport
loss, signal termination or a nonzero remote exit remain conservative unknowns.
The configured executable must run the worker in the foreground, not daemonize.

Hub persists `pending` dispatch intent **before spawning** the transport. It
records `completed` only after observing the foreground command finish. Recovery
uses this journal even if the prior registry snapshot contains no process/PID and
the worker has not connected. Unknown starts participate in shutdown verification
and block redispatch. Older lifecycle records without dispatch status are also
treated as unknown. Losing SSH/Docker connectivity cannot turn an idle probe into
a successful stop, and Hub does not kill the transport merely because the probe
succeeded.

If stop cannot confirm completion it reports `remote-ownership-unconfirmed` (or
`managed-ownership-unconfirmed` when no process record exists). Remote force-kill
is deliberately unsupported in this stage. If the original foreground transport
is still alive, stopping the worker on its target allows completion to be
observed. After that transport is lost, later idle probes alone cannot authorize
restart or prove stop succeeded. An operator may explicitly **delete the session**
to retire its allocation: manager removal refuses a worker holding its lease and
creates a tombstone that rejects delayed starts. Only after confirmed retirement
can Hub kill remaining transports and forget the reference. Deletion is destructive;
it is not an implicit recovery action or part of ordinary stop. Parent session deletion
is refused while descendant allocation intents remain unretired, even after the
parent process exits. Complete child cleanup (or operator recovery) first: an
unpublished child fork still needs the parent's captured configuration to
reconcile preparation and create its retirement tombstone. Non-destructive
recovery of an unknown dispatch requires a future target-side lifecycle fence.

Managed clean-fork uses the captured parent lifecycle and revision; see below.
Legacy subagents retain their existing launch restrictions and storage layout.

### Storage, retries and transition

Hub-local records remain under `dataDir/sessions/<session-id>/`: source and launch
snapshots, `allocation.json`, captured logs, transcript and plan. The target owns
`management.root/<allocation-id>/`: effective configuration reference, state and
memory. Hub reads no target configuration/state files and owns no backend mapper.
The source snapshot is input to preparation, not a copy of the generated target
config reference.

Before the first manager call, Hub atomically publishes and syncs its private
allocation record. Restart reuses the same identity, source and transport even
if the original configuration-library entry has changed or been deleted. Manager
inspection supplies the expected revision; preparation refreshes live connection
endpoints and preserves source settings; startup capture precedes process launch, and durable dispatch intent precedes
transport spawning.
The allocation source/target cannot be changed through the session configuration
replacement API once bound. Create a new session for such changes. This avoids
silently abandoning an old allocation or overwriting its state.

Snapshot inspection returns complete `state` and `readable` values through the
management pipe, with `files: {}` because target paths are not Hub-local artifact
paths. It has no former 8 MiB snapshot cutoff. Missing data is null only when the
manager explicitly reports it; corruption, unreachable peers and malformed replies
fail the HTTP request instead of substituting empty state or a local file.
History and answer queries still use the worker WebSocket protocol.

Deletion (also allowed for a disconnected, unconfirmed managed start) records
cleanup intent locally, verifies target removal through the
manager, and only then deletes this session's Hub-local records. A lost reply or
failed cleanup retains the reference and session for explicit retry. Starting is
blocked while removal is pending. Uncertain initial preparation is reconciled
with the original source/identity before removal; a missing record is not assumed
to prove that an earlier remote operation did nothing. Unrelated Hub files and
other target allocations remain untouched. A corrupt local allocation reference
fails closed rather than selecting legacy local deletion.

Old sessions and launch profiles continue using their existing layout. There is
**no automatic migration or adoption**. Applying managed configuration to a legacy
session with worker artifacts is refused; create a fresh session instead. Do not
copy a generated managed `config.yaml` between directories or delete
`allocation.json` to bypass lifecycle checks. Managed subagent storage and the
deployment/migration audit below are part of the 1.0 release; later changes must
preserve the allocation contract above.

## Managed clean-fork and family recovery

The request envelope identifies the **child** allocation. `source` identifies
its parent's captured startup, on the same configured target/installation. Hub
chooses a fresh UUID and flat target path; model arguments cannot override either.
The manager validates both identities and the captured lifecycle/revision, reads
under a shared parent lease, and publishes under an exclusive child lease. A
running parent may be forked, but a stale lifecycle or occupied child is refused.

Only captured configuration is inherited. The manager rebases known
configuration-relative workspace and plugin paths against the parent's target
config directory. Installation-relative prompt paths remain unchanged. The child
gets `restore: if_present`, its own persistence root, and refreshed event,
confirmation and remote-tool URLs. Confirmation policy starts at `ask`, independently
of the parent. No state, archive, database or WAL file is copied. External
workspaces and operator-configured resources remain shared: clean-fork is not a sandbox.
The manager initially replaces inherited identity URLs with non-routable
`unconfigured.invalid` placeholders; Hub supplies the child's URLs in preparation
before capture and dispatch. Direct CLI callers must likewise provide `refresh`
(or prepare with fresh endpoints) before launching the child.
The inherited source is serialized as JSON (valid YAML); source comments are
retained on the parent, not reproduced in this derived child configuration.

Hub synchronously reserves local ownership and a durable operation receipt before
starting asynchronous manager work. The target stores the fork origin alongside
the child source. Repeating a committed fork uses that source even after parent
retirement; changing its origin conflicts. A failed/lost reply retains the same
allocation ID. Cleanup reconciles preparation before removal and never removes
local bookkeeping until target retirement is confirmed. Tombstones retain only
allocation and fork identities, not credentials/configuration.

Hub child metadata, operation receipts, conversation projections, launch snapshots,
logs and `allocation.json` live under `dataDir/subagents/<generated-id>/`.
Worker data lives separately under `management.root/<allocation-id>/`. Parent
stop/crash freezes descendant admission, waits for startup and owned output,
stops descendants first, then calls manager removal. Cleanup failure retains
ownership and existing bounded retry/backoff/operator-recovery controls. Docker
containers are operator-retained and must remain reachable through removal.

Recovery reads allocation journals even when process publication was interrupted.
A matching local PID/start-time known to have exited can settle a dispatch; stale
lifecycle, missing/unknown identity and remote transport disappearance cannot.
Unconfirmed remote children remain cleanup-pending, even if the manager currently
reports idle. An operator may use the existing lifecycle-correlated
`confirm-terminated` recovery action after verifying termination. The target's
exclusive lease and tombstone are still required before deleting local records.

### Compatibility and direct-I/O audit

No existing directory is migrated automatically. New local templates are managed;
select **Managed worker** for an explicit target/root policy. Keep legacy sessions
on their original profile until finished; create a fresh managed session rather
than applying management to an occupied legacy directory. A managed parent
started by the earlier ordinary-session implementation must be stopped and
restarted once to capture its launch-policy snapshot before clean-fork is available.
Use a target installation that implements `clean_fork`; an older manager reports
unsupported operation and no child is launched.

| Hub area | Managed behavior | Legacy compatibility |
| --- | --- | --- |
| Configuration library/session selection | Hub-local source/provenance and launch policy only | Existing local YAML preparation |
| Supervisor startup and clean-fork | Target prepare/capture/clean_fork; Hub launch snapshot | Existing local startup snapshots |
| Inspector | Full `read_state` result over uncapped stdio; no local fallback | Existing local JSON/readable reads |
| Ordinary deletion and child cleanup | Target `remove`, then local metadata removal | Existing owned local directory cleanup |
| History/answer, plans, transcripts, logs, child receipts | Protocol queries and Hub-local records | Same behavior |

The remaining filesystem accesses in legacy branches are intentional transition
support, not remote fallbacks. A malformed managed allocation record blocks the
operation instead of selecting legacy storage. Hub has no SQLite access or SQL
mapping; backend decoding remains a manager responsibility.

### Deployment acceptance test

`hub/test/e2e/managed-subagents.test.js` is opt-in. Provision an installation of
this revision on a retained target, then pass a management policy via
`SIMPLEX_MANAGED_E2E_POLICY`. The test uses the real C++ worker and manager with
an offline Hub provider, creates nested children, inspects state, and checks
that target removal precedes local bookkeeping removal. It makes no target-data
bind mount and asserts that target allocation paths do not exist on the Hub.

For a Linux retained Docker container using host networking:

```sh
export SIMPLEX_MANAGED_E2E_POLICY='{
  "root": "/var/lib/simplex-test/allocations",
  "executable": "/opt/simplex/bin/simplex",
  "cwd": "/tmp",
  "env": {},
  "target": {"kind":"docker", "command":"docker", "args":[], "container":"simplex-target"}
}'
cd hub
node --test test/e2e/managed-subagents.test.js
```

For SSH, replace `target` with an SSH binding, for example
`{"kind":"ssh","command":"ssh","args":["-p","2222"],"host":"user@127.0.0.1"}`.
This loopback acceptance setup requires the target to reach the Hub's loopback
listeners (for example, SSH into the same host-networked test container). For
separate machines use the deployment profile's advertised endpoint origins and
an accessible provider address; do not expose test listeners to untrusted peers.
The test does not create or retire containers/SSH servers; keep the environment
available through cleanup and remove your test infrastructure afterward.

### Concurrent manager operations

The Hub queues complete preparation and removal sequences per target namespace;
ordinary sessions and headless children share the same queue. Queueing and all
calls in a sequence share a deadline of at most five seconds (or the shorter
configured management/startup timeout). Expired queued work is never dispatched.
Read-only snapshot requests do not enter this queue.

Only a manager-reported `busy` refusal with `published: false` is retried, at
most four calls with short jittered waits. A live worker lease may also produce
`busy`; exhaustion still reports that refusal. Transport/protocol failures,
uncertain outcomes, published failures and other manager codes are not retried.
The target retains its nonblocking locking contract. Equivalent remote targets
expressed through different transport bindings can still contend; the bounded
retry handles that case without claiming to identify every remote alias.
