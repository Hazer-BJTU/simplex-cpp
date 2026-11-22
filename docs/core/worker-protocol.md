# Simplex Loop Worker Protocol

Use this reference to connect a Hub to a worker, trace a tool call, or recover a
conversation view after a disconnect. One worker owns one session and executes
one run at a time.

| Contract | Value |
| --- | --- |
| Worker protocol identifier | `simplex-worker` |
| Manifest format | `simplex.worker-protocol/1` |
| Structural SHA-256, first 12 digits | `3e4ab854c6fa` |

Run `simplex protocol` for the installed worker's complete manifest and
`simplex protocol --hash` for its full fingerprint. The format version describes
the manifest encoding. The fingerprint covers field shapes, presence, message
families, events and vocabularies. Discover available features through
`status.capabilities`. Compare fingerprints for diagnostics rather than using
equality as a connection requirement. [Manifest and JSON Schema](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/protocol/schemas/worker-protocol.json)

## Connections and configuration

Every connection starts at the worker. Send one UTF-8 JSON document per
WebSocket text message. Continue reading the event connection while a separate
connection waits for a decision.

| Worker endpoint | Lifetime | Hub → worker | Worker → Hub |
| --- | --- | --- | --- |
| `client.endpoint` | Persistent, reconnecting | `payload`, `signal` | `event` |
| `security.confirmation.endpoint` | One approval exchange | `confirmation_response` | `confirmation_request` |
| `hub_remote_call.endpoint` | One exchange at an operation-specific route | `tool_response` | `tool_request` |

Payloads, signals and one-shot messages carry their fields in `data`. Events
also carry `event`, `session_id`, `worker_id`, `request_id`, `run_id` and
`sequence` at the top level. Protect configured endpoints and authenticate the
session before accepting input or approvals. The bundled Hub supplies tokenized
routes. [Security boundaries](../architecture/security.md)

## Operations

| Family | `data.operation` | Key input | Observable result |
| --- | --- | --- | --- |
| Payload | `message` | Fresh `request_id`, nonempty ordered `content` | Starts a turn on admission. |
| Payload | `continue` | Fresh `request_id` | Continues from eligible state, including saved compaction memory. |
| Payload | `compact` | Fresh `request_id` | Runs a summary exchange and publishes the replacement state on success. |
| Payload | `history` | `request_id`, optional `start`, `step`, `limit` | Read-only display page or `history_error`. |
| Payload | `answer` | `request_id`, `source`, `part`, `offset` | Read-only exact answer page or `answer_error`. |
| Signal | `status` | None | Current worker state and capabilities. |
| Signal | `options` | None | Available choices and current selections. |
| Signal | `cancel` | Target `run_id` | Requests cooperative cancellation and emits status. |
| Signal | `shutdown` | None | Requests worker shutdown. Observe process exit through the supervisor. |

A content part carries its encoding in `type` (text, binary or external_ref),
media category in `modality`, and value in `raw`. Optional metadata belongs in
`extras`. The three run-producing operations accept an optional `options` object
at admission. Providers and the application interpret option values. History
and answer queries can be served while a run is active.

## Events and correlation

| Events | What the receiver can conclude |
| --- | --- |
| `ready`, `status`, `options` | Initialization finished, current state was reported, or choices were returned. `ready` is emitted once per worker instance. |
| `input_admitted`, `input_rejected` | The request acquired a run ID or was refused. Use `input_rejected.data.request_id` for the rejected input. |
| `run_started`, `input_committed` | The loop admitted the invocation, or a new user message entered in-memory history. |
| `model_response`, `tool_calls`, `tool_results` | A committed model response, proposed batch, or projected batch of results. Results follow call order. Denied and skipped calls can have results. |
| `history`, `history_error`, `answer`, `answer_error` | Query reply. Match the query's own `data.request_id`. |
| `persisted`, `compact_finished` | A checkpoint completed, or compact replacement committed. Invalidate cached history on replacement. |
| `error`, `export_error` | Worker/control diagnostic or optional archive cleanup diagnostic. A terminal storage error carries `durable: false` and the primary outcome. |
| `run_finished` | Logical outcome: `completed`, `cancelled`, `exchange_limit`, or `failed`. Inspect `error`, `exchanges`, and `durable` for the diagnostic, model-response count and final save. |

| Identifier | Scope |
| --- | --- |
| `session_id` / `worker_id` | The session can survive restarts. The worker ID identifies one process incarnation and survives reconnects. |
| `request_id` / `run_id` | The Hub assigns the request ID. The worker assigns a run ID on admission. Event envelopes can refer to the active or most recently admitted run. |
| `sequence` | Worker event counter within one incarnation. Preserve unsigned 64-bit integer precision. |
| `confirmation_id` / call `id` | One approval exchange / one model tool call. Echo approval correlation even when several prompts are pending. |

### Remote tool requests

A tool request carries `worker_id`, `session_id`, `run_id`, its own `request_id`
and an `arguments` object. The route is part of the connection target. The reply
echoes the IDs and route, with either `status: succeeded` and `result`, or
`status: rejected` and `error`. The Hub defines supported routes, arguments,
authorization and results. [Bundled remote tools](https://github.com/Hazer-BJTU/simplex-cpp/tree/main/tools/intrinsic/toolsets/hub_remote_call)

## Delivery and recovery

| Situation | Receiver action |
| --- | --- |
| A send succeeds locally | Wait for admission or rejection. Worker duplicate detection is bounded and process-local. Reconcile uncertain delivery before explicitly submitting another request. |
| Event connection drops | Query status after reconnect. Check the worker ID and run ID. Sequence gaps, omitted previews and coalesced events are possible. Worker sequence numbers cannot request a replay. |
| Cancellation, shutdown, and recovery <a id="cancellation-shutdown-and-recovery"></a> | Allow dispatched tools to settle. A status reply to `cancel` can precede the outcome. Fatal transport or storage failures can end observation before `run_finished`. |
| Continuing after interruption | Inspect `loop.phase`: `ready` or `model` permits eligible work, `projection` applies saved results, and `tools` or `blocked` requires investigation of possible external effects. Restarting alone starts no inference. |

<a id="matching-history-to-event-replay"></a>

### Read a display history page

Read from `start: 0, step: 0`, following the returned `next` and `next_step`
until `next == total` and `next_step == 0`. Keep one worker ID, revision and
total for the entire refresh. Restart the read if they change. Match turns and
steps to observed requests by their execution identity when available.

### Complete answer pages

When a preview carries an `answer_source`, copy it unchanged into an `answer`
query. Start at part and offset zero. Follow the returned part and **UTF-8 byte**
offsets until `done`. Keep parts separate. On a stale source, refresh history
before trying again. This surface returns text and external-reference strings.

<a id="automatic-context-compaction"></a>

### Compact conversation context

Manual compact produces an admitted run. During automatic compact the user
request keeps its run ID through summarization and continuation, with one final
`run_finished` on normal settlement. The worker archives before summarizing.
After a committed replacement, history reflects the new memory. Inspect any
terminal storage error before continuing. [Compaction configuration](../reference/worker-settings.md#loop-and-compaction)

<a id="bundled-hub-implementation"></a>

## Example: one approved tool call

The operator asks the worker for the current directory. These are selected
complete frames from one successful run, with shortened identifiers. Each
JSON block is a separate WebSocket message.

**1. Submit the user message on the event connection.**

```json
{
  "type": "payload",
  "data": {
    "operation": "message",
    "request_id": "q1",
    "content": [
      {
        "type": "text",
        "modality": "text",
        "raw": "Print the working directory."
      }
    ]
  }
}
```

**2. Admission assigns the run ID.** The worker then emits `run_started`,
`input_committed`, a `model_response` containing a call and `tool_calls`.

```json
{
  "type": "event",
  "event": "input_admitted",
  "session_id": "demo",
  "worker_id": "w1",
  "request_id": "q1",
  "run_id": "r1",
  "sequence": 2,
  "data": {
    "operation": "message"
  }
}
```

**3. Receive and answer the approval on its own connection.** Match the session,
worker, run and confirmation IDs. The proposed tool batch and this prompt may
arrive on different sockets in either order.

```json
{
  "type": "confirmation_request",
  "data": {
    "session_id": "demo",
    "worker_id": "w1",
    "run_id": "r1",
    "confirmation_id": "c1",
    "call": {
      "id": "t1",
      "name": "run_command",
      "arguments": {
        "command": "pwd"
      },
      "type": "serial_write",
      "security": "require_confirm"
    }
  }
}
```

```json
{
  "type": "confirmation_response",
  "data": {
    "session_id": "demo",
    "worker_id": "w1",
    "run_id": "r1",
    "confirmation_id": "c1",
    "decision": "approved"
  }
}
```

**4. Observe settlement on the event connection.** The worker commits the tool
results, calls the model again and receives the answer. With a successful
final checkpoint, the terminal event can look like this:

```json
{
  "type": "event",
  "event": "run_finished",
  "session_id": "demo",
  "worker_id": "w1",
  "request_id": "q1",
  "run_id": "r1",
  "sequence": 14,
  "data": {
    "status": "completed",
    "error": "",
    "exchanges": 2,
    "durable": true,
    "task_exchanges": 2,
    "compact_exchanges": 0,
    "auto_compact": {
      "attempts": 0,
      "succeeded": 0,
      "limit": 5,
      "threshold": 0
    },
    "rejected_queries": 0,
    "unreported_rejections": 0
  }
}
```

Admission correlates the input, approval authorizes the call, and
`run_finished` settles the request. The bundled Hub exposes these observations
to operator clients through its [UI SDK](../hub/ui-sdk.md).
