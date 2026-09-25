# Worker application

One process owns one session, one model, both registries, an IO client and at
most one active agent loop. simplex_worker assembles all intrinsic components,
all provider descriptors and configured dynamic components. simplex_shell is
a separate one-to-one terminal server, not a multi-worker hub.

## Start

Build simplex_worker and simplex_shell, or install the complete project.
Copy bin/config.example.yaml to an operator-owned config.yaml. Select the
provider/model and set credentials. If config.yaml is outside bin, copy the
`prompts` directory alongside it or set an absolute `worker.system_prompt_file`.
Omitting that field uses the default prompt beside the executable. For the local example:

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
  system_prompt_file: ./prompts/coding_agent.yaml
persistence:
  directory: ./data/sessions
  readable: false
~~~

The default structured prompt lives in [core/prompts/coding_agent.yaml](prompts/coding_agent.yaml)
and is copied/installed as `bin/prompts/coding_agent.yaml`. The loader resolves
an explicit relative path against the startup configuration file's directory.
It validates the file at startup, even when restoring a session. New sessions
use its sections; restored sessions retain the prompt in their snapshot. Tool
skills are rebuilt from the active registry in both cases. See the
[load prompt format](../load/README.md#system-prompt-files) for custom files.

Run in separate terminals inside a disposable container when testing process
tools:

~~~sh
simplex_shell --listen 127.0.0.1:8765
simplex_worker --config ./config.yaml --session demo --threads 4
~~~

`simplex_worker --help` prints the available options. `--config/-c` defaults
to `config.yaml`; `--session/-s` is required. `--threads/-t` accepts a positive
integer and defaults to 1. It counts all threads running the worker io_context,
including the main thread; it does not increase the number of active agent loops.
All executor threads are joined before the application is destroyed.

The shell uses plain local WebSockets without authentication, for a trusted
local test environment. Production deployments supply an authenticated service;
the worker supports wss://. See [the shell guide](example/README.md).

Session IDs accept 1–128 ASCII letters, digits, underscores or hyphens.
Snapshots use <persistence.directory>/<session>/state.json; optional Markdown
uses readable.md in the same directory. New sessions use the configured system
prompt; restoration retains the stored prompt and history. Startup and reconnect
never start a run automatically.

## Communication protocol

See the [formal worker client protocol](docs/worker-protocol.md) for all message
formats, event data, connection roles, confirmation decisions, delivery limits,
and recovery behavior. Message payloads carry an ordered `content` array of
text/attachment parts. Use `external_ref` for image URLs; optional `extras`
preserves additional metadata for future richer modalities.
It is the reference for independently implemented hubs
and clients. The [documentation index](docs/index.md) lists the package's formal
documents and their publishing conventions. The `options` signal returns
available choices and current selections for model and confirmation, plus a
reserved tools category, in the normal event metadata envelope. Hubs can query
it after reconnecting; the query does not change settings.

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

core_worker_container runs the actual shell, worker, provider adapter and
process tools against an offline SSE fixture. It exercises approval with
separate stdout/stderr and cancellation while waiting for confirmation.
core_lifecycle_container uses actual workers to verify session ownership,
crash recovery with executed children, historical process-ID rejection, and
shutdown with descendant-held pipes. Both tests are skipped outside Docker.
The Linux test_core_dns fixture blocks an already-running resolver backend
until explicitly released, covering both timeout and external cancellation.
 A manual real-provider session
requires deployment credentials and remains a separate operator check.

## Build dependencies

The top-level core package composes load, loop, IO, intrinsic process tools and
the intrinsic context-statistic hook. core_protocol contains only message
validation/identities and links the dataclass and Boost header interfaces.
The shell links core_protocol and intercom_iface, without worker or provider
implementation dependencies. intercom_exchange supplies a cancellable one-shot
transport with an overall deadline. load now parses full worker settings in
addition to its independent plugin-discovery and explicit persistence APIs.

Runtime environment hints can be supplied through `worker.environment`:
`workspace` and `platform` strings and a `software` string list. These describe
expected conditions without changing the working directory or enforcing access
restrictions. At startup, including restore, current hints replace the
`environment.runtime` Volatile section after tool skills and before other
Volatile sections. Empty configuration removes old hints. See
[configuration and lifecycle details](../load/README.md#runtime-environment-hints).
