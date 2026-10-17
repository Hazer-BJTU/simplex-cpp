# Worker storage management

`simplex manage` runs a target-local management program installed beside the
worker. It prepares and inspects managed allocations and removes them after
worker shutdown. This initial implementation supports local JSON storage.
Hub launch profiles can opt new ordinary sessions into this interface using
`management`. Existing profiles remain on their legacy layout; subagent
clean-fork migration follows in [#142](https://github.com/Hazer-BJTU/simplex-cpp/issues/142).

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
| `read_state` | None | Returns `{backend: "json", state, readable}`. Missing state/readable exports are null; corruption is an error. Complete state, including compatible unknown JSON fields, is retained. |
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
the later launcher adapter must order refresh, capture and launch consistently.

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

The protocol currently reports `unsupported_backend` for non-JSON storage.
SQLite support in [#187](https://github.com/Hazer-BJTU/simplex-cpp/issues/187) will
read through the C++ backend and return the same JSON state shape. Hub code must
not acquire database dependencies or read database files directly.

## Hub launcher transport boundary

The Hub's internal `ManagedLauncher` adapter provides the next integration layer.
It binds `simplex run` and `simplex manage` to one explicitly configured execution
target and installation. The **Managed worker** template in the configuration editor enables it for
new ordinary sessions. Existing profiles and sessions remain on their legacy
launch path. Child clean-fork and recovery integration follow in #142.

The adapter supports these invocation shapes:

| Target | Invocation | Lifecycle prerequisite |
| --- | --- | --- |
| Local | `simplex manage --request BASE64` | Installed `simplex` supports management. |
| SSH | `ssh [connection options] -T -- HOST 'simplex' 'manage' '--request' 'BASE64'` | Target uses a POSIX command shell; worker ownership must be verified independently of SSH exit. |
| Docker | `docker [context options] exec CONTAINER simplex manage --request BASE64` | Container already exists and remains running through worker shutdown and allocation removal. |

SSH quotes every remote argument, including executable paths containing spaces or
apostrophes. Docker uses argument arrays with neither a shell nor a PTY. There is
no inference from legacy arbitrary command templates, implicit container creation,
or shared-directory fallback. The existing foreground `docker run --rm` launch
pattern cannot provide post-exit management access; integration must introduce an
explicit container lifetime before switching that path.

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

Create a launch configuration using **Managed worker**, edit it, and select it
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
  "launcher": { "kind": "command", "command": ["simplex", "run"] },
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

When `management` is present, its executable/target replaces the legacy
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
{ "kind": "docker", "command": "docker", "args": ["--context", "my-target"], "container": "simplex-target" }
```

The operator creates and keeps that container running independently of the
worker process (for example, an image with `simplex` installed and an idle init
process). Hub runs both commands using `docker exec`, with no bind mount needed.
The container must remain available after worker exit until deletion succeeds.
Hub does not create, stop or remove this container. Its lifecycle must not use a
foreground worker as PID 1 with automatic container removal. Configure advertised
Hub endpoints reachable from the target as with other remote launch profiles.

Remote stop sends the normal shutdown signal, then verifies the manager can
acquire an exclusive allocation lease. Losing SSH/Docker CLI connectivity is not
proof of remote exit. If the target cannot be verified idle, stop reports
`remote-ownership-unconfirmed`; Hub does not signal a remote PID on its own host
or kill the entire shared container. Remote force-kill is deliberately unsupported
in this stage. Stop the worker on the target and retry stop/delete if protocol
shutdown is unavailable. A restarted Hub retains a remote ownership record even
when the former transport PID no longer exists, until an explicit stop can verify
that the allocation is idle. Managed clean-fork currently returns
`unsupported_launch`; legacy subagents are unchanged until phase C.

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
endpoints and preserves source settings; startup capture precedes process launch.
The allocation source/target cannot be changed through the session configuration
replacement API once bound. Create a new session for such changes. This avoids
silently abandoning an old allocation or overwriting its state.

Snapshot inspection returns complete `state` and `readable` values through the
management pipe, with `files: {}` because target paths are not Hub-local artifact
paths. It has no former 8 MiB snapshot cutoff. Missing data is null only when the
manager explicitly reports it; corruption, unreachable peers and malformed replies
fail the HTTP request instead of substituting empty state or a local file.
History and answer queries still use the worker WebSocket protocol.

Deletion records cleanup intent locally, verifies target removal through the
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
`allocation.json` to bypass lifecycle checks. Phase C completes managed subagent
storage and the final deployment/migration audit.
