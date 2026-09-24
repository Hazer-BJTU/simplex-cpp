# Core worker implementation plan

Status: implemented initial worker and terminal example. See README.md for the
current contract, startup settings, explicit limits and validation. The original
design below records the architecture; real-provider manual validation still
requires operator credentials.

## Application boundary

One process is one worker with one session runtime and at most one active
`loop::run()`, including cancellation and tool-batch drain. The worker owns its
model instance, `ToolRegistry`, `LoopHookRegistry`, synchronous EventBus, process
tool session store, IO client, and `AgentInputState`. Runtime resources are not
serialized; the dataclass remains the persistent conversation/recovery object.

`core` coordinates these resources and their lifetime. `load` handles startup
configuration, plugin discovery/construction, and explicit state persistence.
`loop` owns model/tool iteration and state transitions. Tools retain their
existing security checks; they do not gain dependencies on network routing or
the terminal UI.

The first runnable host will use:

```text
simplex_worker --config ./config.yaml --session demo
simplex_shell --listen 127.0.0.1:8765
```

The explicit session ID selects restoration across process restarts. Validate it
before deriving any path. Proposed layout is
`<persistence.directory>/<session-id>/state.json`, with an optional `readable.md`
export. The CLI starts the application; it does not implement a second agent
loop. Process signals enter the same controlled shutdown path as remote stop.

## Planned package structure

```text
core/
  include/core/
    application.hpp       worker lifecycle and host-facing entry points
    protocol.hpp          application messages and correlation identities
  src/
    application.cpp       resource assembly and supervised coroutines
    protocol.cpp          message validation and event encoding
    confirmation.cpp      asynchronous EventBus-to-endpoint adapter
    main.cpp              CLI, process signals, and exit codes
  example/
    shell_server.cpp      one-worker WebSocket endpoint router
    terminal.cpp          serialized plain-terminal input/output
    README.md
  test/
```

Keep protocol code usable by the example without requiring provider or tool
implementation libraries. The final public types should follow actual ownership
needs; this plan does not freeze speculative request/dependency wrappers.

## Startup and session assembly

1. Complete `load` parsing for provider endpoints, `driver_model`, the event
   client, persistence policy, and the proposed confirmation endpoint settings.
   Preserve tolerant handling of unknown fields and provider-defined options.
   Resolve explicit relative paths against the configuration file.
2. Load all compatible provider descriptors and the selected dynamic products.
   Construct all intrinsic components using their existing configuration rules.
   There is no public intrinsic enable/disable or configuration override.
3. Construct the selected driver model, expanding only its required credentials.
   Establish registries and verify duplicate names and unavailable components.
4. Load or initialize state. New sessions receive the system prompt, tool
   catalogue, and injected skills. Restored sessions retain history and hook
   state; reconcile current capabilities without rewriting historical calls.
   Old process handles do not become live operating-system children on restore.
5. Install scoped event subscriptions and the authoritative confirmation adapter
   before tool execution can begin. Register intrinsic hooks before configured
   dynamic hooks, preserving their specified ordering.
6. Start the event client, one payload consumer, and one application-event sender.

Initial system-prompt selection and loop exchange budget still need small,
explicit configuration/CLI decisions during implementation. Do not silently
reuse the disposable-container persona from the existing interactive example.

## Event and confirmation endpoint configuration

Retain `client.endpoint` for the existing persistent, bidirectional channel.
It carries incoming payloads/signals and outgoing worker events. Introduce a
separate security request/reply endpoint rather than putting approval decisions
back into that channel's signal queue:

```yaml
client:
  endpoint: ws://127.0.0.1:8765/agent/events
  # Existing queue capacities and transport options remain here.

security:
  confirmation:
    endpoint: ws://127.0.0.1:8765/agent/confirm
    timeout_ms: 120000
```

These URLs are independent and may point to different hosts. Missing confirmation
configuration leaves `RequireConfirm` calls unapproved, with a clear diagnostic;
it never implies automatic approval. A configured but malformed URL or invalid
timeout is a startup error. `timeout_ms` must be positive and measures the entire
confirmation operation, not only socket inactivity. Neither this field nor the
endpoint's path changes the model-provider endpoint configuration.

For the shell example, matching server CLI route options should be explicit:

```text
simplex_shell --listen 127.0.0.1:8765 \
  --events-path /agent/events --confirmation-path /agent/confirm
```

The event route upgrades to one long-lived connection. The confirmation route
upgrades to a separate, single-request/single-reply WebSocket session. This
reuses the intercom request/reply transport shape and permits a future security
service to answer independently of the UI/event connection. The first version
supports WebSocket confirmation endpoints; an HTTP implementation would be a
separate transport adapter, not an implicit interpretation of this contract.

## Application protocol

Preserve incoming `{"type":"payload","data":...}` and
`{"type":"signal","data":...}` envelopes. Payload operations initially include
new messages and explicit continuation. Validate user-message types before loop
admission; arbitrary payloads must not impersonate tool results. Signals cover
cancellation, status requests, and worker shutdown. Cancellation identifies a
specific run, so a stale signal cannot stop the next one.

Outgoing events identify the session, input request, run, and event sequence.
The initial event set includes readiness/status, input admission, run start/end,
model responses, tool calls/results, errors, and persistence outcomes. Initially
send complete model responses; token-level streaming can follow separately.

Confirmation messages are a different request/reply contract, for example:

```json
{
  "type": "confirmation_request",
  "data": {
    "session_id": "demo",
    "run_id": "run-1",
    "confirmation_id": "confirmation-7",
    "call": {
      "id": "call-3",
      "name": "run_command",
      "arguments": {"command": "pwd"}
    }
  }
}
```

```json
{
  "type": "confirmation_response",
  "data": {
    "session_id": "demo",
    "run_id": "run-1",
    "confirmation_id": "confirmation-7",
    "decision": "approved",
    "reason": "Approved by the operator."
  }
}
```

The actual call payload includes the settled invocation metadata produced by
`ensure_arguments()` and `write_attributes()`, including type and security.
Validate all reply correlation fields and the decision enum. Only an explicit,
matching approval may approve the event. Malformed, mismatched, duplicate, late,
or disconnected responses cannot authorize execution. Generate a new confirmation
ID for every confirmation attempt; call IDs alone may repeat across runs.

The first implementation performs one confirmation attempt without transparent
retries. Repeating a request can duplicate an operator prompt. Future retry
support requires a documented idempotency/correlation contract at both ends.

Queue admission, application acceptance, and durable completion are separate
outcomes. Reconnect never automatically resends an input that may have run.
The shell can query status on reconnect. Request IDs support correlation and
explicit duplicate handling, not an unqualified exactly-once promise.

## Asynchronous confirmation listener

Subscribe one host-owned coroutine listener to `tools::InvokeConfirmEvent` on
the asynchronous bus used by the tools. The process-wide default async bus is
the existing common integration point, including plugins using the default
security helper; pass that same bus to intrinsic process tools explicitly.
Keep its scoped subscription alive until all dispatched batches and confirmation
operations have completed.

The listener copies only request/correlation data, obtains the current run's
confirmation scope, and awaits a request/reply exchange to the configured
endpoint. It returns the original event with `Approved` or `Denied` and a reason.
It must not alter the settled query or access mutable conversation state from a
network callback. Confirmation scope state is process-local, not serialized.

The async event bus folds replies in subscription order, and a later subscriber
can overwrite an earlier decision. The host therefore owns one authoritative
confirmer on this integration path; do not install competing confirmation
listeners. A plugin using its own security implementation or private bus must
explicitly participate in this contract; the host cannot infer control over it.

Reuse `intercom` connection/exchange primitives, but verify cancellation over
DNS resolution, TCP/TLS connection, upgrade, write, reply read, and close. Existing
`fetch_once()` documents an inactivity timeout, not an overall hard deadline,
and its graceful close is part of the await. Calling it alone does not establish
the required cancellation/deadline guarantees. Add the minimum cancellable
exchange support in `intercom` if needed, with an explicit abort path owned by
the exchange executor. Do not solve this with an abandoned coroutine or by
stopping an io_context that other work still uses.

## Cancelling a loop while confirmation is pending

Do not cancel `ToolRegistry::execute()` as a whole. Instead, treat the pending
security exchange as separately cancellable work within a batch that still
must finish. Each admitted run gets a fresh stop source and confirmation scope.
Parallel tool branches may have several pending confirmations in that scope.

The confirmation state machine is process-local:

```text
Pending -> Approved
Pending -> Denied (operator denial, cancellation, timeout, or request failure)
```

Terminal decisions cannot be changed. A short mutex-protected arbitration step
serializes approval with cancellation. It protects only the scope's stopping
flag and decision bookkeeping; no lock spans an await, callback, or network IO.

When a cancel request arrives:

1. Verify its run identity. Close admission to new confirmations in that run and
   mark all still-pending decisions denied under the control lock. Any listener
   starting after this point immediately returns a cancellation denial.
2. Release the lock. Request the loop stop token and post abort/cancellation for
   each affected exchange to its owning executor. Do not emit Asio cancellation
   signals or manipulate sockets directly from the signal worker thread.
3. Await network-operation and deadline-task completion. The confirmation
   listener returns `Denied` with a cancellation reason after its owned work is
   joined. This permits the existing tool security gate to return its normal
   non-execution result.
4. Let the registry settle all branches and let the loop commit their results.
   Save the completed cancellation state before reporting cancellation complete.

If cancellation wins arbitration, a later approval is ignored. If approval wins,
the call has passed the confirmation gate and belongs to the admitted tool batch;
later cancellation does not revoke it or interrupt its side effects. This is an
explicit boundary, not a promise that no tool can begin after a cancel signal
arrives. A stronger pre-invocation guarantee would require a separate tool
dispatch contract change and is not part of this plan.

Use a separate cancellation source/slot for each pending exchange. Loop-level
shielding of inherited cancellation must not shield the confirmation transport:
the adapter supervises that operation explicitly. An already-cancelled scope
must not start another request. The overall deadline stays armed through graceful
close; expiry follows the same denied/abort/join path. The adapter still awaits
cleanup and does not promise an exact wall-clock completion bound under executor
starvation. Human interaction must never be required to complete cancellation
or worker shutdown.

No server cancellation acknowledgement is required for local termination. The
server drops or marks a prompt expired when its confirmation socket closes; a
later UI action cannot reopen that decision. The server does not execute the
tool, so closing this request leaves execution authority at the worker.

## Executor and queue ownership

One serialized execution context owns conversation state. A synchronous EventBus
does not imply one thread: `io::Client` publishes signals from its own worker
thread. Signal listeners use the thread-safe run control described above or
post commands; they never read or edit `AgentInputState` directly.

Loop observers copy only event payloads needed for output into a bounded
application-event queue. They must not await `client.send()` or retain borrowed
event references. A dedicated sender owns asynchronous queue admission to the
WebSocket client. Define a visible failure policy for queue exhaustion: request
a safe run stop, retain local state, and report the communication problem rather
than dropping critical results silently or allocating an unbounded backlog.

Separate network progress from potentially slow synchronous serialization where
needed, while keeping state access serialized. Do not hand a borrowed live
state to a background writer while the loop continues modifying it.

## Persistence boundaries and restoration

JSON is authoritative; Markdown is an optional derived export. Connect the
existing save policy to stable boundaries, not to an arbitrary last edit-hook
subscriber. Edit hooks can still fail final validation and be rolled back.

Proposed minimal loop notifications:

- A read-only step-finished event after `EditOnStepFinished` passes validation.
- A read-only pre-dispatch checkpoint after phase becomes `Tools` and before
  any registry branch executes. Existing `BeforeToolBatch` still sees `Ready`
  and is not sufficient to record possible tool side effects.
- A read-only results-ready checkpoint after the complete batch enters
  `pending_results` in `Projection`, before conversation projection.

The last two checkpoints, when persistence is enabled, prevent automatic replay
of ambiguous calls after a crash and preserve already-returned results for
projection recovery. Define observer-failure handling so a failed pre-dispatch
save cannot fall through into tool execution. Step/final save controls do not
implicitly remove these safety checkpoints; document their distinct purposes
when integrating the startup policy. Persistence disabled means no crash-recovery
guarantee. Keep the existing recursive JSON checking policy for now.

Use existing `RunFinished` only as a read-only final boundary. Because it logs
observer failures without changing the final loop result, `core` must track and
inspect storage failure separately before reporting durable success or admitting
another run. Preserve error evidence and do not continue after a failed required
JSON checkpoint. A failed Markdown export does not invalidate saved JSON.

Restoration never starts a run automatically. `Tools`/`Blocked` require operator
inspection, and `Projection` recovery uses the stored results without executing
the tools again. Recreate runtime registries and connections, not process handles
or old confirmation requests. Do not treat an interrupted confirmation as a
persisted approval on the next process run.

## One-to-one terminal server

The example server accepts HTTP upgrade requests and routes by configured path
before upgrading. Reject unknown paths and conflicting route configuration.
Only one worker event connection is active at a time; reject a second one and
allow reconnect after disconnection. Multiple transient confirmation connections
for that same worker are allowed, because a batch may request several approvals.
This is still one worker with multiple request sessions, not a multi-worker hub.

The event route displays worker events and sends user payloads/control signals.
The confirmation route validates a correlated request, queues a terminal prompt,
and sends exactly one decision back on that request's socket. Associate requests
with the example's logical worker/session, retaining that identity across event
reconnection, and expire prompts on confirmation-socket closure or deadline.
Do not admit an unrelated replacement worker while the previous session has
pending prompts. Endpoint services are independent: losing a confirmation connection
must not tear down the event channel. Define event-reconnect behavior without
automatically resubmitting inputs or approvals.

The network accept/read loops must not wait for terminal input. Serialize console
output; multiplex normal input, `/continue`, `/cancel`, `/status`, shutdown, and
ID-addressed approval/denial commands. Several pending prompts must remain
distinguishable, and cancellation must be available while a prompt is displayed.
Separate assistant output, reasoning, tool calls, results, stdout, and stderr.
Use plain text formatting; no browser frontend or generic hub is included.

## Supervision and shutdown

On shutdown, stop application input admission, close confirmation admission,
deny pending confirmations, and request the active loop's stop. Join confirmation
exchanges and the current tool batch; commit results and perform final saving.
Terminate and reap owned process-tool children while their executor still runs.
Attempt a bounded final event send, stop the IO client, and await its transport,
signal worker, and application sender before releasing subscriptions, registries,
models, buses, and executors. Network delivery is not required for local cleanup.

Fatal transport or sender errors enter the same supervised path. Keep one owner
of shutdown and preserve the primary failure alongside any cleanup diagnostics.
Never release an async confirmer subscription while a batch can still invoke it.

## Implementation stages and acceptance tests

1. **Configuration and protocol:** complete the needed `load` parsing; define
   message identities, endpoint routes, and errors; test independent endpoint
   selection, unknown-field tolerance, and invalid/absent confirmation settings.
2. **Confirmation adapter:** bridge the existing async event, with a test
   endpoint and controllable transport. Cover approve/deny, malformed and wrong-ID
   replies, disconnection, timeout, pre-cancellation, parallel pending calls,
   cancellation during connect/write/read/close, and late replies. Deterministic
   race tests prove one terminal decision and no orphaned tasks.
3. **Worker runtime:** assemble intrinsic/dynamic components and the model;
   supervise payload, signal, and outbound work. Test single active loop,
   model cancellation, tool drain, confirmation cancellation, stale run IDs,
   queue exhaustion, and graceful/failing shutdown.
4. **Recovery and storage:** add precise checkpoint notifications and connect
   saving policy. Verify edits are saved only after validation, pre-dispatch IO
   failure prevents execution, crash-phase restoration never replays uncertain
   tools, and Markdown failure leaves JSON recovery usable.
5. **Terminal example:** implement both routed endpoints and asynchronous console
   interaction. Test one-worker admission, reconnect, simultaneous confirmation
   requests, expired prompts, endpoint isolation, and separated output streams.
6. **Integrated validation:** use an offline model fixture and real process tools
   in disposable containers. Exercise the full user-message/approval/tool/result
   path and cancel a pending approval without human intervention. Then perform
   a manual real-provider container session and document install/startup usage.

The later Node.js hub reuses these endpoint and application contracts. Worker
routing, multi-user UI, and a web frontend are deferred to that project.
