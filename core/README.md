# Worker application

One process owns one session, a driver model, an optional modality-assist model,
both registries, an IO client and at most one active agent loop. simplex_worker
assembles all intrinsic components,
all provider descriptors and configured dynamic components. The public command
entry point is `simplex run`; the [hub](../hub/README.md) provides the server and
interactive panel.

The optional top-level `modality_assist_model` configuration names an entry in
`providers`, just like `driver_model`. Core constructs a separate instance at
startup and keeps shared ownership for the application lifetime. Omission skips
construction; an invalid or unavailable configured model fails startup before
client admission. A loaded conversation model enables the intrinsic
[`modality_assist` toolset](../tools/intrinsic/toolsets/modality_assist/README.md),
which sends local images in isolated requests and returns text to the driver.
The auxiliary model is not part of `AgentInputState`, and the current
`options.model` protocol continues to configure only the driver. See the
[model-role configuration](../load/README.md#providers-and-model-roles).

## Start

Build the `simplex` target (`cmake --build build --target simplex`), or build and
install the complete project with CMake. The build tree and installation both
provide `bin/simplex` beside `bin/simplex_worker`. Add the installed `bin` directory
to `PATH` to use the public command.
Copy bin/config.example.yaml to an operator-owned config.yaml. Select the
provider/model and set credentials. `worker.system_prompt_file` is relative to
the executable's directory, not to config.yaml: a configuration kept anywhere
still reads the installed prompt, and an absolute path is rejected. Omitting the
field uses the default prompt beside the executable. When using
the bundled hub, use its generated session configuration for authenticated
per-session endpoints. For another server exposing the following routes:

~~~yaml
client:
  endpoint: ws://127.0.0.1:8765/agent/events
security:
  confirmation:
    endpoint: ws://127.0.0.1:8765/agent/confirm
    timeout_ms: 120000
worker:
  max_exchanges: 512
  event_capacity: 1024
  system_prompt_file: prompts/coding_agent.yaml
persistence:
  directory: ./data/session
  state: state
  readable: false
  memory: memory
~~~

The default structured prompt lives in [core/prompts/coding_agent.yaml](prompts/coding_agent.yaml)
and is copied/installed as `bin/prompts/coding_agent.yaml`. The loader resolves
an explicit relative path against the executable's own directory and refuses a
rooted path (a leading separator in either grammar, or a drive letter) and `..`
components; the containment is lexical, so symlinks are the filesystem's
business. It validates the file at startup, even when restoring a session. New sessions
use its sections; restored sessions retain the prompt in their snapshot. Tool
skills are rebuilt from the active registry in both cases. See the
[load prompt format](../load/README.md#system-prompt-files) for custom files.

Start the hub or another compatible server, then run the worker. Use a
disposable container when testing process tools:

~~~sh
simplex run --config ./config.yaml --session demo --threads 4
~~~

`simplex --help` lists subcommands; `simplex run --help` prints the available options. `--config/-c` defaults
to `config.yaml`; `--session/-s` is required. `--threads/-t` accepts a positive
integer and defaults to 1. It counts all threads running the worker io_context,
including the main thread; it does not increase the number of active agent loops.
All executor threads are joined before the application is destroyed.

The launcher requires Bash and forwards arguments unchanged using `exec`.
The caller's working directory and environment are preserved, as are the worker's
exit status and signal handling. It finds the worker beside the launcher,
including when invoked through a symbolic link, so a complete installed tree
can be relocated. Worker configuration defaults still resolve from the caller's
working directory; plugin and default prompt discovery remain relative to the
worker executable. The underlying `simplex_worker` binary remains available for
programmatic launchers such as the hub.

`simplex_shell` is deprecated: its source and historical tests remain for
reference, but it has no build target, install rule, or `simplex` subcommand.
See [the deprecated example](example/README.md).

Session IDs accept 1–128 ASCII letters, digits, underscores or hyphens.
Snapshots use `<persistence.directory>/<persistence.state>/state.json`; optional Markdown
uses readable.md in the same directory. `persistence.directory` is the direct
session root; the worker never appends a session ID. The `state` and `memory`
subdirectories default to `state` and `memory` and must be relative paths without
parent traversal. The root holds `session.lock`, shared by every state
subdirectory configuration. New sessions use the configured system
prompt; restoration retains the stored prompt and history. Startup and reconnect
never start a run automatically.

## Communication protocol

See the [formal worker client protocol](../docs/core/worker-protocol.md) for all message
formats, event data, connection roles, confirmation decisions, delivery limits,
and recovery behavior. Message payloads carry an ordered `content` array of
text/attachment parts. Each part states its encoding (`type`: `text`, `binary`,
`external_ref`) and its media category (`modality`: `text`, `image`, `audio`,
`video`, `document`) explicitly; an image URL is
`{"type": "external_ref", "modality": "image"}`, never a category inferred from
the encoding. Adapters map the categories their provider can describe and reject
the rest, so optional `extras` carries provider metadata rather than a
substitute label.
It is the reference for independently implemented hubs
and clients. The [documentation index](../docs/core/index.md) lists the package's formal
documents and their publishing conventions. The `options` signal returns
available choices and current selections for model and confirmation, plus a
reserved tools category, in the normal event metadata envelope. Hubs can query
it after reconnecting; the query does not change settings.
The `history` payload queries a simplified, paged projection of the current
in-memory turns. It can be answered during a model wait and does not start a
run or reveal the full restorable snapshot. The worker protocol specifies its
cursor and clipping limits.

## Confirmation and cancellation

The worker owns one authoritative InvokeConfirmEvent subscription on the
default asynchronous bus. Another existing confirmer is a startup error.
Private plugin security buses are outside this contract. Missing endpoint
configuration denies calls requiring confirmation in the default `ask` mode,
with a diagnostic. A payload may set `options.confirmation.mode` to `ask`,
`approve`, or `deny` for that run and subsequent runs. Local approval/denial
performs no network IO; approval still respects run cancellation. `Trusted` and
`DefaultDeny` retain their tool-layer meaning. Model and confirmation options are
validated together at admission, cannot change an active run, and are not
persisted. The `options` signal reports choices and selections without
modifying them.
Any party allowed to submit payloads can select automatic approval. Production
Hubs must authorize that payload channel as an approval authority; correlation
IDs are not credentials. See the formal protocol for deployment requirements.

In `ask` mode, confirmation uses one independent text WebSocket exchange without retries.
The request has type=confirmation_request and data containing worker_id,
session_id, run_id, a fresh confirmation_id, and the settled call (including
security, type and normalized arguments). The response has
type=confirmation_response and echoes all four IDs with
decision=approved|denied and optional reason.
Malformed, mismatched, binary, disconnected or expired replies deny execution.

The overall deadline spans DNS, TCP/TLS, upgrade, write, read and graceful
close. Cancellation closes confirmation admission, aborts transport on its
own strand, and joins the exchange and timer before returning a denial.
Approval and cancellation arbitrate under a short mutex. Approval that wins
belongs to the admitted batch; cancellation does not revoke it or interrupt
tool side effects. The registry drains and the loop commits complete results.

Application::run() is single-use and is the lifetime fence. Keep the object
and executor alive until completion. stop() and the stop token request
controlled shutdown; neither stops the executor. Shutdown cancels pending
approvals/model work, drains the batch, saves state, terminates and reaps owned
process children, and joins all owned process pipe tasks before joining the
sender and IO client. After reaping, explicit process shutdown allows 100 ms
for output drainage, then closes remaining pipes even if descendants hold
inherited descriptors. Captured bytes are retained, unfinished streams are
marked truncated, and undelivered stdin is discarded. Normal completion still
drains output naturally. This does not implement descendant-tree termination. Final outbound admission
is attempted for at most 500 ms; delivery is not required for cleanup.
The first fatal error propagates after cleanup. Process cleanup retains watcher
and signal failures, stops and joins owned tasks, attempts recovery reaping,
and visits every session before reporting the first error. If the OS refuses
termination/reaping, cleanup still closes owned pipes and joins the watcher;
it reports failure without claiming that the child exited.


The deadline is an authorization cutoff, not a hard wall-clock bound on return.
A system DNS backend already inside `getaddrinfo` may not be interruptible.
Expiration or cancellation still invalidates the reply, but completion (and
worker shutdown waiting for it) can be delayed until that backend returns.
The operation retains and joins this work; it never detaches resolution or
allows a late result to revive approval. Numeric endpoint addresses avoid DNS
lookup when bounded resolver latency is required.

## Persistence and recovery

Creation time is minted for new sessions. updated_at tracks host admission,
validated step/final edits and orderly shutdown. Read-only recovery checkpoints
preserve the last logical edit timestamp; they do not mutate borrowed state.

JSON is saved before dispatch with phase=tools, after complete results enter
phase=projection, and after validated step edits when configured. Final and
shutdown saves follow their policy flags. Cancellation additionally saves
settled state even when on_run_finished is disabled. Safety checkpoints are
mandatory whenever persistence is enabled, independent of step/final flags.

A required JSON failure latches a fatal storage error. No later save overwrites
the last recovery file, and no next run is admitted. Post-rename directory-sync
failure also counts: the new file may be visible but durability is uncertain.
Markdown failures are separate and do not invalidate saved JSON. Only JSON
previews are bounded in Markdown; ordinary text remains complete.

Tools/Blocked snapshots require operator inspection and never replay calls.
Projection recovery uses buffered results without redispatch. Startup
reconciles current tool definitions and skills but retains historical calls.
Runtime process handles and old approvals are not restored. A persisted
process ID remains auditable in conversation history but never aliases a new
process: each process store has a fresh UUID namespace. IDs are opaque.

Before inspecting or restoring a snapshot, a persistent worker acquires an
exclusive nonblocking advisory lock on `session.lock` in the session directory.
A duplicate start fails before admission or snapshot changes. Ownership lasts
through final save and cleanup; startup exceptions and process death release
it. The descriptor is close-on-exec, so executed tools cannot retain the lock.
The lock file must never be removed/replaced while workers may use it. All
writers must cooperate, using the same local POSIX filesystem with `flock`
support. Network/distributed filesystems are outside this ownership contract.
Different session directories are independent.

## Validation

The core configuration, confirmation and application tests use local peers
and offline models. They cover protocol validation, decisions/correlation,
disconnect/deadline/cancellation, serial admission, duplicates, stale run IDs,
model cancellation, event overflow, storage failure and blocked restoration.
Loop tests verify checkpoint failure, projection recovery and validated edits.

`core_simplex_cli` runs worker option and SIGTERM checks through `simplex run`.
`core_simplex_launcher` checks routing, argument boundaries, relocation, symlinks,
working directory, environment and exit/PID preservation. The hub end-to-end
suite covers a real worker with tool approval against an offline provider.
`core_lifecycle_container` uses actual workers to verify session ownership,
crash recovery with executed children, historical process-ID rejection, and
shutdown with descendant-held pipes; it is skipped outside Docker.
The Linux test_core_dns fixture blocks an already-running resolver backend
until explicitly released, covering both timeout and external cancellation.
 A manual real-provider session
requires deployment credentials and remains a separate operator check.

## Build dependencies

The top-level core package composes load, loop, IO, intrinsic process/reading/editing tools and
the intrinsic context-statistic hook. core_protocol contains only message
validation/identities and links the dataclass and Boost header interfaces.
intercom_exchange supplies a cancellable one-shot
transport with an overall deadline. load now parses full worker settings in
addition to its independent plugin-discovery and explicit persistence APIs.

Runtime environment hints can be supplied through `worker.environment`:
`workspace` and `platform` strings and a `software` string list. These describe
expected conditions without changing the working directory or enforcing access
restrictions. At startup, including restore, current hints replace the
`environment.runtime` Volatile section after tool skills and before other
Volatile sections. Empty configuration removes old hints. See
[configuration and lifecycle details](../load/README.md#runtime-environment-hints).

## Explicit context compaction

The `compact` payload archives the current state as Markdown, generates a
structured summary without tools, and replaces conversation turns with a final
`memory.runtime` system-prompt section. The worker publishes the replacement JSON
snapshot before emitting `compact_finished`. Failed or cancelled attempts retain
the original conversation. See [the wire contract](../docs/core/worker-protocol.md#compact-conversation-context)
for admission rules, failure behavior, and event fields. The hub exposes it as
**Compact context** in Command mode, gated on the current worker's capabilities.

Archives accumulate under `<persistence.directory>/<persistence.memory>`
(default subdirectory `memory`), without another session-ID component. Every attempt reserves a new numbered directory, so
restarts and clock changes preserve ordering without replacing earlier files.
After a successful compact, `persistence.memory_retention` applies a
`max_archives` count limit (5 by default; zero disables cleanup). The current archive is
always retained. Failed attempts and unfamiliar files may remain; these are
cleanup targets rather than a disk quota. Cleanup failure is reported alongside
the saved summary without undoing it.
Injected memory identifies the absolute archive directory for later tool-based
lookup. The compact instruction is loaded from
[prompts/operations/compact.yaml](prompts/operations/compact.yaml) at startup.

## Optional hub remote-call toolset

`hub_remote_call.endpoint` and optional `timeout_ms` (default `120000`) enable an
intrinsic `HubRemoteCallToolSet` with the `plan` tool. `construct_runtime()` copies these settings
into the set before registering it. Omission leaves it unloaded. The set registers the plan tool and skill without opening a startup connection.
See the [package guide](../tools/intrinsic/toolsets/hub_remote_call/README.md) for
the request base and [wire protocol](../docs/core/worker-protocol.md#remote-tool-requests).
