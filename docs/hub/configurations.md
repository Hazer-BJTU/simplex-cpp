# Configuration library and session snapshots

The panel manages two kinds of reusable configuration source files. **Launch
configuration** describes how the Hub starts a process. **Worker configuration**
is the worker's YAML startup document. A session selects one of each and retains
its own copies. Saving a reusable file does not change an existing session.

## Editing the library

For initial setup, follow [worker configuration](../getting-started/configuration.md)
and [Hub deployment](../deployment/hub.md). This page defines the library's
editing, storage, and API behavior.

**New from template** copies editable source into the editor. Enter a name and
save to publish it. IDs contain 1–128 letters, digits, underscores or hyphens;
file extensions are automatic. Editing the name and saving creates a copy.
**Rename** preserves the source under a new name. **Delete** removes only the
library file; sessions retain their snapshots. **Reload** discards local edits
with confirmation and reads the latest saved revision.

Templates are installation examples, not copies of the running Hub configuration.
Fill in provider settings and target-specific paths explicitly. Local and Managed
worker templates use a single `management.executable`: `simplex` on the target
PATH, or an absolute path to that router. The same installation runs both worker
and management commands; no separate `worker.bin` or `launcher.command` is needed.
Existing saved launch profiles remain supported and are never rewritten.
Auxiliary model and remote-call settings remain commented out until enabled.

Validation checks syntax, selected model references, managed endpoint fields
and persistence child paths. Unknown worker fields and provider options remain
intact. The worker still validates plugin availability and runtime resources.
Validation failures leave the editor text and the saved file unchanged.

New **local** launch templates use `simplex manage` automatically. Their
`management.executable` selects the router used for both run and inspection;
`management.cwd` selects its working directory. Allocations live under
`<dataDir>-workers/<allocation-id>`, outside Hub-owned data. Existing library and
session snapshots are never rewritten to enable management.

The **Docker (running container)** template targets a container the operator
already runs; the **Managed worker** template additionally supports explicit
local roots and SSH. Both use `management`; see
[managed sessions](../reference/management.md#opt-in-managed-sessions) for the
policy and lifecycle contract. Existing library files and session snapshots are
never rewritten.

## Directory and startup configuration

The default root is `~/.simplex/hub`, resolved using the Hub process's home
directory. `--data-dir /absolute/path` selects another root. Existing data is
not moved automatically.

```text
<dataDir>/
├── hub.config.jsonc
├── hub.json                  # Hub session metadata
├── configs/
│   ├── launch/
│   │   ├── local.jsonc
│   │   └── docker.jsonc
│   └── worker/
│       └── default.yaml
├── sessions/
│   └── <session-id>/
│       ├── config/
│       │   ├── launch.jsonc
│       │   ├── config.yaml
│       │   ├── source.json
│       │   ├── startup-worker.yaml   # legacy profiles; managed sessions write startup-launch.jsonc
│       │   └── startup-launch.jsonc
│       ├── state/state.json     # legacy JSON worker state
│       ├── memory/<archive>/    # JSON archive; SQLite archives live in its DB
│       ├── logs/worker.log
│       ├── events.jsonl
│       ├── session.lock
│       └── plan.json
└── subagents/
    └── <generated-id>/          # flat roots; parents are recorded in metadata
        ├── metadata.json
        ├── operations.json
        ├── conversation.json   # bounded primary dialogue, no event transcript
        ├── config/
        │   ├── launch.jsonc
        │   ├── config.yaml
        │   ├── startup-worker.yaml
        │   └── startup-launch.jsonc
        ├── state/
        ├── memory/
        ├── logs/
        ├── session.lock
        └── plan.json
```

This shows default paths, not files guaranteed to exist before first use.
Library-selected sessions have `source.json`; supervised launches capture the
`startup-*` files for clean-fork. State, archives, logs and plans are written
only when their corresponding features produce them. A legacy `readable.md` from
an older worker may still be present; the worker no longer writes one.
Worker state/memory subdirectories may be changed in its saved configuration.

The CLI first reads an explicit `--config` file, or looks for
`hub.config.jsonc` (then `hub.config.json`) inside the selected root. When no
startup file exists, it uses defaults plus CLI overrides and writes the resolved
configuration there on first CLI startup. Subsequent CLI overrides do not
rewrite an existing file. Explicit external `--config` files remain supported.
Relative paths in an explicit startup file resolve against that file's directory;
CLI path overrides resolve against the invocation directory.

Hub listener, authentication and resource-limit settings remain startup settings
in `hub.config.jsonc`; changing them requires restarting the Hub. The browser
editor manages launch and worker library files. It does not change a running
Hub's listeners. Library files and newly published snapshots use private file
permissions. The Hub process owns the configuration library; use one Hub process
per root. Revisions protect concurrent panel edits within that process.

`local`, `docker` and `default` are seeded when absent at startup. Other library files
are never replaced by startup. Worker templates are bundled from
`load/schemas/config.example.yaml`, with a regression check preventing drift.

The commented [Hub startup example](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/hub/hub.config.example.jsonc)
covers the validated key paths; its coverage is checked against `defaultConfig()`.
Provider profile contents are owned by each model plugin and remain open-ended.
In particular, API-created sessions without library selectors use these defaults:

| Startup key | Default | Effect on new generated worker configurations |
| --- | --- | --- |
| `worker.maxExchanges` | `512` | Ordinary loop exchange allowance |
| `worker.autoCompactThreshold` | `0` | Last-exchange total-token threshold; zero disables automatic compact. When enabled, exchange allowance exhaustion also triggers it. |
| `worker.maxAutoCompactions` | `5` | Positive maximum of successful automatic compactions per user request |

Per-session `autoCompactThreshold` and `maxAutoCompactions` override these initial
defaults. They map to worker YAML `worker.auto_compact_threshold` and
`worker.max_auto_compactions`; they are not transport byte ceilings. Existing saved
`config.yaml` files are reused. A session using a worker library file takes loop
policy from that YAML rather than these startup defaults; edit its `worker` section
and apply the file while stopped. Compact itself is exempt from the ordinary
threshold/exchange allowance and never recursively triggers compact. See the
[automatic compact contract](../core/worker-protocol.md#automatic-context-compaction)
for final-answer priority, cancellation and the per-request loop guard.

## Launch configuration

See [`schemas/local.jsonc`](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/hub/schemas/local.jsonc) for a commented example.

| Field | Meaning |
| --- | --- |
| `launcher.kind` | `command` for an argument-array template; `simplex-worker` for direct binary invocation |
| `launcher.command` | Executable and separate arguments for `command`; no implicit shell |
| `launcher.args` | Additional arguments |
| `launcher.cwd` | Command working directory; empty uses the session directory; relative paths resolve against the session's `config/` directory |
| `launcher.pidFile` | Absolute PID-file path for a daemonizing launcher, if applicable |
| `management` | Managed allocation policy: target, installation and allocation root; replaces the launcher for supported targets (see below and [managed sessions](../reference/management.md#opt-in-managed-sessions)) |
| `worker.bin` / `worker.args` | Direct worker executable and flags for `simplex-worker`; explicit relative binary paths resolve against the session's `config/` directory |
| `worker.threads` | Execution thread count; positive integer |
| `worker.connectHost` | Host reachable from the worker; empty derives from Hub listeners |
| `worker.stopTimeoutMs` | Graceful shutdown deadline |
| `worker.sigtermGraceMs` / `worker.sigkillGraceMs` | Escalation grace periods |
| `env` | String environment overrides; other variables are inherited from the Hub |
| `endpoints` | Optional `events`, `confirm`, `tools` WebSocket origins, including proxy prefixes |

Command placeholders retain the existing launcher contract: `{session}`,
`{config}`, `{data_dir}`, `{session_dir}`, `{endpoint}`, `{confirm_endpoint}`,
`{tools_endpoint}`, `{token}`, `{threads}`, `{worker_bin}`, `{uid}`, `{gid}`.
Only the command and argument arrays expand these placeholders. Environment
values are literal strings. Use the inherited environment for model secrets.

For a managed container target, `localhost` refers to the container. Set
`worker.connectHost` (or a channel `endpoints` override) to an address reachable
from that network, such as the Docker bridge gateway or a mapped
`host.docker.internal`. The startup check rejects a loopback address the container
cannot use unless it shares the host network. The Hub must listen on an address
reachable from that network; no browser-side heuristic can determine it
reliably.

See [advertised endpoints and reverse proxies](../deployment/hub.md#advertised-endpoints-and-reverse-proxies)
for origin/prefix examples and listener routing.

## Docker management template

The seeded `docker` configuration starts workers inside a container that the
operator already runs. It contains a `management.target` with
`"kind": "docker"`, the container name and a `preflight` block. The Hub never
builds an image or creates, starts, stops or removes a container; it only runs
`simplex manage` and `simplex run` there with `docker exec`. See
[Docker worker deployment](../deployment/docker-worker.md) for the image,
retention, network and troubleshooting details.

| Field | Meaning |
| --- | --- |
| `management.target.container` | Existing running container; it must outlive every session that uses it |
| `management.target.command`, `args` | Docker CLI and global options such as `--context`; never an image or a `run` command |
| `management.target.preflight` | Read-only startup checks: `enabled`, `timeoutMs`, `container`, `executable`, `endpoint` |
| `management.executable` | PATH name or absolute path **inside the container**; must support `run` and `manage` |
| `management.root` | Container-local allocation root; state is durable only while the container lives |
| `management.cwd`, `env` | Hub-local Docker CLI directory and environment |

Before the first target mutation the Hub checks the container state, runs
`executable --version` inside it, and inspects whether the advertised event
endpoint can be used from the container's network namespace. A failed check
refuses the start; a new session stays unbound, so both configuration files can
still be edited before retrying. `"preflight": {"enabled": false}` skips every check; each check
has its own boolean switch. Existence is always checked: `container: false`
skips only the running assertion, and a stopped container still refuses an
installation probe. These overrides apply to a new session before it binds an
allocation; a bound session keeps its launch policy.
`management.target.preflight.endpoint.channels` selects the advertised channels
to inspect (`events`, `confirm`, `tools`).

A launch profile saved by an older release is not converted automatically.
Create a new launch configuration from this template instead; existing library
files and session snapshots are never rewritten.

## Worker templates and managed fields

The template includes these markers:

```yaml
client:
  endpoint: '{{hub.events_endpoint}}'
security:
  confirmation:
    endpoint: '{{hub.confirm_endpoint}}'
# hub_remote_call:
#   endpoint: '{{hub.tools_endpoint}}'
persistence:
  directory: '{{session.directory}}'
```

At launch, the Hub replaces these endpoint fields and the persistence root with
session-specific values. Literal values in these same fields are refreshed too;
configure connection origins in the launch file. This is field-based YAML
editing, not global text substitution. Provider URLs, API keys, unknown fields
and comments are preserved. `${ENV_VAR}` remains for the worker to interpret.

Omitting `hub_remote_call` keeps remote tools disabled. Omitting
`security.confirmation` keeps confirmation unconfigured. Omitting
`modality_assist_model` keeps the assistant model disabled. The Hub does not
re-enable these optional sections on restart.

For legacy launch profiles, `persistence.directory` is always `<dataDir>/sessions/<session-id>` for a
Hub-managed worker. `persistence.state` and `persistence.memory` are relative
child directories without parent traversal. The worker adds no session ID.
Changing the state subdirectory does not move old state into the new location.

## Session lifecycle

Creating a session with `launchConfig` and `workerConfig` validates both files,
copies them into the session directory and records their source IDs and
revisions in `source.json`. Each session builds its own launcher from its saved
launch configuration. Starting or restarting reads those snapshots, refreshes
managed fields and starts the process. Library files are no longer needed to
restart the session; renaming or deleting them does not break it.

The published snapshot is the authority for the two selected configuration
IDs. If the Hub stops between publishing a replacement and updating `hub.json`,
restore reconciles the session description from `source.json`. If `hub.json` is
lost or corrupt, an unregistered session ID may be created again through the
normal API; creation replaces any orphaned config snapshot for that ID. This
creates a new session token. Stop any old worker before recreating its ID if
the Hub lost its bookkeeping while that worker may still be running.

To update an existing **legacy** session, stop its worker and wait for disconnection. In
**Configurations**, choose both saved files and select **Apply to …**. This
replaces the session's configuration snapshot; it does not delete conversation
state, archives or plans. Unsaved editor changes are not applied. Model and
prompt changes follow the worker's normal restore semantics: changing a prompt
file does not replace a system prompt already restored from persisted state.

Managed sessions bind their source and execution target once; replacing that
snapshot is refused after allocation. Create a new session to change source or
target. If Hub registry metadata is lost, an existing `allocation.json` prevents
silently reusing its session ID; retain/recover that ownership record rather than
replacing the orphaned snapshot. Automatic legacy migration is not supported.
Managed subagent clean-fork inherits the captured startup and execution target,
with fresh target storage; see [management](../reference/management.md#managed-clean-fork-and-family-recovery).

Existing API-created sessions without configuration selectors retain the legacy
startup-file defaults and saved `config.yaml` behavior. They can opt into the
library by explicitly applying a pair of configurations while stopped.

## Configuration API

All routes use the same authentication and origin checks as other panel APIs.
Source text is returned only by explicit configuration reads, not session lists.
Requests carrying source text are bounded to 1 MiB of JSON request body.

| Method and route | Body / result |
| --- | --- |
| `GET /api/configurations` | `{launch: string[], worker: string[]}` |
| `GET /api/configurations/:kind/template?source=default` | `{text}`; source is `default`, `docker` or `managed` (last two are launch only) |
| `GET /api/configurations/:kind/:id` | `{kind, id, text, revision}` |
| `PUT /api/configurations/:kind/:id` | `{text, revision}`; `revision: null` creates, current revision updates |
| `POST /api/configurations/:kind/validate` | `{text}` → `{valid: true}` |
| `DELETE /api/configurations/:kind/:id` | `{revision}` → `{removed}` |
| `POST /api/configurations/:kind/:id/rename` | `{id, revision}` → updated file record; existing destinations are refused |
| `POST /api/configurations/preview` | `{launch: sourceText}` → `{endpoints: {events, confirm, tools}}`, using illustrative session identity |
| `POST /api/sessions/:id/configurations` | `{launchConfig, workerConfig}` → `{session}`; `409` if running, connected, or already bound to a managed allocation |

Create a session through REST or the panel WebSocket with:

```json
{
    "session": "project-notes",
    "spec": { "launchConfig": "local", "workerConfig": "default" }
}
```

REST uses `POST /api/sessions`; WebSocket adds `"type": "create_session"`.
Revision mismatches return HTTP `409` without overwriting source. Keep the local
draft, reload the saved source and reconcile changes before trying again.

## Headless delegation

The Hub supports clean-fork/send/receive remote routes for directly owned headless
workers. Each child uses a flat `<dataDir>/subagents/<generated-id>/` root, an
independent operator-controlled ask/deny/approve policy, and a bounded primary
conversation projection. It retains no event transcript or reasoning/tool history.
Parent process shutdown/crash cascades; socket disconnect alone preserves the
family. Confirmed child shutdown deletes its persistence. Clean-fork shares any
explicit external workspace and is not a sandbox. The intrinsic `hub_remote_call`
toolset supplies `subagent_fork`, `subagent_send` and `subagent_receive` when the
worker's remote endpoint is configured.
See the [complete subagent contract](subagents.md) for configuration snapshots,
launch support, request deduplication, recovery and cleanup limits.


New worker templates select `persistence.backend: sqlite`. A managed allocation
stores `state/state.sqlite3` on its target, not under the Hub's data directory.
Existing saved configurations retain their backend; legacy `format: json` keeps
JSON. Neither configuration refresh nor clean-fork copies/migrates state.
Fresh native API sessions without selectors also bind the router beside
`worker.bin` when SQLite persistence is enabled, using `<dataDir>-workers`.
Existing worker files, explicit JSON API sessions and custom command launchers
retain their prior path. SQLite snapshot inspection requires management; legacy
local inspection supports JSON only and explicitly diagnoses unsupported SQLite.
