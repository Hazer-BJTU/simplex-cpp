# Worker application

One process owns one session, one model, both registries, an IO client and at
most one active agent loop. simplex_worker assembles all intrinsic components,
all provider descriptors and configured dynamic components. simplex_shell is
a separate one-to-one terminal server, not a multi-worker hub.

## Start

Build simplex_worker and simplex_shell, or install the complete project.
Copy bin/config.example.yaml to an operator-owned config.yaml. Select the
provider/model and set credentials. For the local example:

~~~yaml
client:
  endpoint: ws://127.0.0.1:8765/agent/events
security:
  confirmation:
    endpoint: ws://127.0.0.1:8765/agent/confirm
    timeout_ms: 120000
worker:
  max_exchanges: 12
  event_capacity: 256
  system_prompt: You are a helpful assistant. Follow the available tool guidance.
persistence:
  directory: ./data/sessions
  readable: false
~~~

Run in separate terminals inside a disposable container when testing process
tools:

~~~sh
simplex_shell --listen 127.0.0.1:8765
simplex_worker --config ./config.yaml --session demo
~~~

The shell uses plain local WebSockets without authentication, for a trusted
local test environment. Production deployments supply an authenticated service;
the worker supports wss://. See [the shell guide](example/README.md).

Session IDs accept 1–128 ASCII letters, digits, underscores or hyphens.
Snapshots use <persistence.directory>/<session>/state.json; optional Markdown
uses readable.md in the same directory. New sessions use the configured system
prompt; restoration retains the stored prompt and history. Startup and reconnect
never start a run automatically.

## Input and output protocol

Incoming envelopes preserve the IO package's payload/signal routing:

~~~json
{"type":"payload","data":{"operation":"message","request_id":"input-1","text":"Hello"}}
{"type":"payload","data":{"operation":"continue","request_id":"input-2"}}
{"type":"signal","data":{"operation":"cancel","run_id":"<run UUID>"}}
{"type":"signal","data":{"operation":"status"}}
{"type":"signal","data":{"operation":"shutdown"}}
~~~

Payloads cannot provide roles, tool results or call metadata. One consumer
executes payloads serially. Signals are independent and post onto the
state-owning strand. A stale run ID never cancels a later run.

Outgoing objects have type=event, event, session_id, worker_id, request_id,
run_id, sequence, and data. Worker/run UUIDs distinguish process restarts;
sequence increases within a worker lifetime. Events include ready, status,
input_admitted, input_committed, input_rejected, run_started, model_response,
tool_calls, tool_results, persisted, export_error, error and run_finished.
Responses are complete messages, not token deltas. Correlation fields describe
the active or last run; input_rejected also names the rejected request in data.

Queue admission is not peer receipt, application admission or durable
completion. A bounded cache rejects duplicate IDs among the most recent 4096 admitted
requests in this worker lifetime. Older entries are evicted. This cache is not
persisted and does not promise exactly-once execution outside that window or
across restarts.
Clients must inspect status/state instead of automatically replaying inputs
with unknown outcomes. The shell discards inputs submitted while disconnected.

The bounded application-event queue fails the worker on exhaustion. It requests
safe cancellation and preserves local state instead of silently dropping
results. Transport admission is separately bounded. status.rejected_payloads
reports inbound IO queue rejection. Connection failures follow the existing
stable-client policy, including indefinite connect-stage retries.

## Confirmation and cancellation

The worker owns one authoritative InvokeConfirmEvent subscription on the
default asynchronous bus. Another existing confirmer is a startup error.
Private plugin security buses are outside this contract. Missing endpoint
configuration denies calls requiring confirmation, with a diagnostic.

Confirmation uses one independent text WebSocket exchange without retries.
The request has type=confirmation_request and data containing session_id,
run_id, a fresh confirmation_id, and the settled call (including security,
type and normalized arguments). The response has type=confirmation_response
and echoes all three IDs with decision=approved|denied and optional reason.
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
The first fatal error propagates after cleanup.


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
