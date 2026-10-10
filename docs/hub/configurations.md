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

The **Current Hub deployment** template source captures settings from the
Hub's startup configuration. This provides a transition for existing Docker,
custom-command and mock-provider deployments: create and save both a launch
file and a worker file from that source, then select them when creating a
session. With `--mock`, the deployment worker template selects the mock provider;
its live model endpoint is refreshed at worker launch. Default templates remain
local-launch and provider-neutral examples regardless of mock mode. Auxiliary
model and remote-call settings remain commented out until explicitly enabled.

Validation checks syntax, selected model references, managed endpoint fields
and persistence child paths. Unknown worker fields and provider options remain
intact. The worker still validates plugin availability and runtime resources.
Validation failures leave the editor text and the saved file unchanged.

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
└── sessions/
    └── <session-id>/
        ├── config/
        │   ├── launch.jsonc
        │   ├── config.yaml
        │   └── source.json
        ├── state/
        ├── memory/
        ├── logs/
        ├── events.jsonl
        ├── session.lock
        └── plan.json
```

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

## Launch configuration

See [`schemas/local.jsonc`](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/hub/schemas/local.jsonc) for a commented example.

| Field | Meaning |
| --- | --- |
| `launcher.kind` | `command` for an argument-array template; `simplex-worker` for direct binary invocation |
| `launcher.command` | Executable and separate arguments for `command`; no implicit shell |
| `launcher.args` | Additional arguments |
| `launcher.cwd` | Command working directory; empty uses the session directory; relative paths resolve against the session's `config/` directory |
| `launcher.pidFile` | Absolute PID-file path for a daemonizing launcher, if applicable |
| `launcher.dockerManagementEnv` | Additional environment **names** required by Docker inspection/signaling; defaults to `[]`; their startup values are persisted privately |
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

For containers, `localhost` refers to the container. Set `worker.connectHost`
to a host reachable from the container, such as a configured Docker host gateway.
The Hub must listen on an address reachable from that network. No browser-side
heuristic can determine that address reliably.

See [advertised endpoints and reverse proxies](../deployment/hub.md#advertised-endpoints-and-reverse-proxies)
for origin/prefix examples and listener routing.

## Docker launch template

The seeded `docker` configuration uses `simplex-worker:latest`, a local Docker
daemon, and the installed `simplex run` entry point. Its complete image, network,
mount, credential, and ownership setup is documented in
[Docker worker deployment](../deployment/docker-worker.md).

The in-tree `simplex-hub-test:latest` image uses the build-tree launcher
`/src/build/bin/simplex`; when selecting that image, replace the separate
`simplex` command argument with that path and keep `run` as the next argument.
The template does not build an image or install a worker automatically.

### Docker management environment

Docker startup inherits the Hub environment plus launch `env` overrides, so
`docker run -e MODEL_API_KEY ...` still forwards that credential. Later inspection
and shutdown use the captured absolute Docker executable, working directory and a
smaller environment. Only these built-in names and explicit pass-throughs are
saved in `hub.json` and headless `metadata.json`:

| Purpose | Preserved names |
| --- | --- |
| Docker daemon, context, configuration and TLS | `DOCKER_HOST`, `DOCKER_CONTEXT`, `DOCKER_CONFIG`, `DOCKER_CERT_PATH`, `DOCKER_TLS`, `DOCKER_TLS_VERIFY`, `DOCKER_API_VERSION`, `DOCKER_CUSTOM_HEADERS` |
| Executable/home lookup and SSH agent | `PATH`, `HOME`, `USER`, `LOGNAME`, `SSH_AUTH_SOCK` |
| Proxy and system trust roots | `HTTP_PROXY`, `HTTPS_PROXY`, `NO_PROXY`, `http_proxy`, `https_proxy`, `no_proxy`, `SSL_CERT_FILE`, `SSL_CERT_DIR` |
| Windows host lookup | `SYSTEMROOT`, `SystemRoot`, `WINDIR`, `USERPROFILE`, `HOMEDRIVE`, `HOMEPATH`, `APPDATA`, `LOCALAPPDATA`, `PATHEXT` |

The connection variables follow the [Docker CLI environment contract](https://docs.docker.com/reference/cli/docker/#environment-variables).
No wildcard such as `DOCKER_*` is retained. A special CLI/helper deployment can
declare extra names in its launch configuration:

```json
{
  "launcher": {
    "dockerManagementEnv": ["CUSTOM_MANAGER_TOKEN", "XDG_RUNTIME_DIR"]
  }
}
```

This fragment supplements the other launcher fields. Names must be unique and
match `[A-Za-z_][A-Za-z0-9_]*`; values come from the effective startup environment,
without interpolation. A listed name absent at startup stays absent after restart.
Do not list model credentials forwarded only to the container. The declaration
is retained in session/startup snapshots, including clean-forks, independently of
later library or Hub defaults.

Recovery filters legacy full-environment snapshots to the built-in names; new
snapshots also retain their saved pass-through declarations. Recovery never fills
missing values from the current Hub environment. Legacy deployments requiring
extra helper variables should configure the list and relaunch to capture it.
Missing or invalid context remains unverified; it does not authorize deletion.
Necessary proxy/header credentials and explicitly selected secrets can still be
stored in these mode-0600 files. Old disk copies are not securely erased; protect
the data directory and backups. Explicit credentials already placed in launch
`env` remain part of that operator-authored configuration.

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

`persistence.directory` is always `<dataDir>/sessions/<session-id>` for a
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

To update an existing session, stop its worker and wait for disconnection. In
**Configurations**, choose both saved files and select **Apply to …**. This
replaces the session's configuration snapshot; it does not delete conversation
state, archives or plans. Unsaved editor changes are not applied. Model and
prompt changes follow the worker's normal restore semantics: changing a prompt
file does not replace a system prompt already restored from persisted state.

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
| `GET /api/configurations/:kind/template?source=default` | `{text}`; source is `default`, `deployment`, or `docker` (launch only) |
| `GET /api/configurations/:kind/:id` | `{kind, id, text, revision}` |
| `PUT /api/configurations/:kind/:id` | `{text, revision}`; `revision: null` creates, current revision updates |
| `POST /api/configurations/:kind/validate` | `{text}` → `{valid: true}` |
| `DELETE /api/configurations/:kind/:id` | `{revision}` → `{removed}` |
| `POST /api/configurations/:kind/:id/rename` | `{id, revision}` → updated file record; existing destinations are refused |
| `POST /api/configurations/preview` | `{launch: sourceText}` → `{endpoints: {events, confirm, tools}}`, using illustrative session identity |
| `POST /api/sessions/:id/configurations` | `{launchConfig, workerConfig}` → `{session}`; `409` if running or connected |

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
explicit external workspace and is not a sandbox. No C++ subagent tool ships yet.
See the [complete subagent contract](subagents.md) for configuration snapshots,
launch support, request deduplication, recovery and cleanup limits.
