# Worker storage management

`simplex manage` runs a target-local management program installed beside the
worker. It prepares and inspects managed allocations and removes them after
worker shutdown. This initial implementation supports local JSON storage.
Existing Hub launchers have **not** switched to this interface yet; automatic
remote management, Hub-local data separation, and subagent clean-fork integration
are subsequent work in [#142](https://github.com/Hazer-BJTU/simplex-cpp/issues/142).

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
