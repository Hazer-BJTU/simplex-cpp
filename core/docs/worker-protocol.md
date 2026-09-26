# Worker client communication protocol

This is the communication contract implemented by `simplex_worker`. It is
intended for authors of hubs, gateways, terminal clients, and web interfaces in
any language. The bundled `simplex_shell` is one example server, not a required
implementation or an additional protocol layer.

The worker is always the WebSocket client. One worker process owns one session
and executes at most one agent-loop invocation at a time. A hub may accept many
workers, but must route each connection and confirmation to the correct worker
and session. This document describes the current protocol, including its
limitations; it does not introduce unimplemented acknowledgements or recovery
endpoints.

## Contents

- [Connections and configuration](#connections-and-configuration)
- [Encoding and message envelopes](#encoding-and-message-envelopes)
- [Identifiers and correlation](#identifiers-and-correlation)
- [User requests](#user-requests)
- [Control signals](#control-signals)
- [Worker events](#worker-events)
- [Shared data types](#shared-data-types)
- [Tool confirmation](#tool-confirmation)
- [Ordering and example exchanges](#ordering-and-example-exchanges)
- [Delivery, backpressure, and reconnects](#delivery-backpressure-and-reconnects)
- [Cancellation, shutdown, and recovery](#cancellation-shutdown-and-recovery)
- [Hub implementation requirements](#hub-implementation-requirements)

## Connections and configuration

There are two independent connection roles:

| Role | Worker configuration | Lifetime | Traffic |
| --- | --- | --- | --- |
| Events and inputs | `client.endpoint` | Persistent, reconnecting | Hub sends payloads and signals; worker sends events. |
| Tool confirmation | `security.confirmation.endpoint` | One connection per confirmation | Worker sends one request; hub sends one response; worker closes. |

Both are WebSocket endpoints, not ordinary HTTP POST handlers. They may use
separate paths on the same server or different servers. A hub must allow
confirmation connections while the event connection is open, including multiple
simultaneous confirmations from one tool batch. The following is a configuration
fragment; provider/model configuration is also required to start a worker:

```yaml
client:
  endpoint: wss://hub.example.com/agent/events
  payload_capacity: 256
  signal_capacity: 256
  transport:
    write_capacity: 256
    initial_backoff_ms: 250
    max_backoff_ms: 10000
    idle_timeout_seconds: 0
security:
  confirmation:
    endpoint: wss://hub.example.com/agent/confirm
    timeout_ms: 120000
worker:
  event_capacity: 1024
  max_exchanges: 512
  system_prompt_file: ./prompts/coding_agent.yaml
persistence:
  enabled: true
  directory: ./data/sessions
  format: json
  restore: if_present
  readable: false
  save:
    on_step_finished: true
    on_run_finished: true
    on_shutdown: true
```

The numeric and boolean values above are the defaults. Endpoints have no
implicit hub address; the event endpoint is required. Omitting the confirmation
configuration denies calls that require confirmation. Queue capacities,
backoff delays, confirmation timeout, and `max_exchanges` must be positive.
`max_backoff_ms` must be at least `initial_backoff_ms`. The idle timeout is
nonnegative; zero disables it.

`worker.system_prompt_file` selects an independent YAML prompt file for new
sessions. Explicit relative paths resolve against the main configuration file.
Omitting it reads `prompts/coding_agent.yaml` beside the executable. The file is
validated at startup, including when restoring a session; missing or malformed
files fail startup. Restored sessions retain their stored prompt. Current tool
skills and configured `worker.environment` hints are refreshed in both cases.
Environment hints describe the workspace, platform, and expected software without
changing the working directory or restricting access. See
[environment configuration](../../load/README.md#runtime-environment-hints) and the [prompt file format](../../load/README.md#system-prompt-files).
The previous inline `worker.system_prompt` field is rejected.

Paths such as `/agent/events` and `/agent/confirm` are examples, not reserved
protocol routes. URLs accept `ws://` and `wss://`, an explicit or scheme-default
port, and a path/query. Userinfo, fragments, whitespace, and invalid ports are
rejected. The configured target, including its query, is used for the upgrade.

For `wss://`, the worker uses TLS peer and hostname verification with the system
trust paths. For `ws://`, traffic is plaintext. The current worker configuration
does not expose custom WebSocket authentication headers, cookies, client
certificates, or subprotocol negotiation. Provider API credentials configure
model requests only; they are not sent as hub credentials. `session_id`,
`worker_id`, and `run_id` are correlation identifiers, not authentication
credentials. Deployment authentication and routing must account for these limits
rather than assume an authentication exchange.

**Payload access grants confirmation-policy authority.** Any party permitted to
submit a payload can select `confirmation.mode: approve`; that party can then
authorize every `RequireConfirm` tool call in that run and later runs without
an interactive prompt. Treat this as granting approval authority, not merely
permission to send a user message. Production deployments **must authenticate
and authorize access to the worker-facing event/payload channel** before
exposing automatic approval. The Hub must enforce that boundary, including for
browser and other downstream clients. Do not connect an untrusted browser or
client directly to that endpoint. Use plain `ws://` with automatic approval
only within a trusted environment; use `wss://` when transport protection is
needed. TLS encrypts the connection but does not by itself authorize who may
send payloads. This worker does not implement its own client authentication
handshake; the deployment must provide the required trust boundary.

The transport applies 30-second TCP-connect, TLS-handshake, and WebSocket-upgrade
deadlines to their respective stages, not one combined 30-second deadline.
A running system DNS resolver may take longer to finish. An established event
connection has no idle timeout by default. With a positive idle timeout, Beast
uses WebSocket ping/peer-response handling; this is not an application JSON
heartbeat. Hubs must implement normal WebSocket control frames. No `ping` or
`pong` JSON operation is defined.

## Encoding and message envelopes

Each application message is one complete WebSocket **text** message containing
one UTF-8 JSON value. WebSocket fragmentation is handled by the transport; JSON
is parsed only after the whole message has arrived. Do not send newline-delimited
records, multiple JSON documents in one message, or binary application messages.
A `Content` value with `type: "binary"` is base64 inside JSON and still travels
in a text WebSocket message.

Hub-to-worker event-connection messages have this envelope:

```json
{"type":"payload","data":{"operation":"message","request_id":"req-001","content":[{"type":"text","raw":"Hello"}]}}
```

`type` must be `payload` or `signal`; `data` must be present. Each operation
below requires an object as `data`. Unrecognized extra envelope fields are
ignored. The worker emits events in the following envelope:

```json
{
  "type": "event",
  "event": "run_started",
  "session_id": "demo",
  "worker_id": "204d23ea-0f10-4dbb-b2ef-613bc3f7852d",
  "request_id": "req-001",
  "run_id": "fd473c9f-f424-4c5a-bcc7-eb39698674de",
  "sequence": 3,
  "data": {}
}
```

Every listed envelope field is always emitted. `data` depends on the event; it
can be an object or an array. Events without additional data carry `{}`.

There is no protocol-version field or version-negotiation exchange. A compatible
hub should tolerate unknown extra fields and preserve opaque `extras` values.
It should not disconnect merely because a future worker emits an unfamiliar
event name. This receiver guidance does not imply that the current worker
accepts unknown input types or operations.

## Identifiers and correlation

| Field | Origin and scope | Interpretation |
| --- | --- | --- |
| `session_id` | Worker startup `--session` | 1–128 ASCII letters, digits, `_`, or `-`; selects the persistent session. May survive worker restarts. |
| `worker_id` | Worker-generated UUID | New for each worker application instance; unchanged by reconnects. |
| `request_id` | Hub | Nonempty string of at most 128 UTF-8 bytes. Identifies an admitted message/continuation within the worker's bounded duplicate window; for `history`, it only correlates a read-only response and is not cached. |
| `run_id` | Worker-generated UUID | New for each admitted request, including `continue`. Required to target cancellation. |
| `sequence` | Worker | Unsigned 64-bit event counter, starting at 1 and increasing across reconnects within this worker instance. |
| `confirmation_id` | Worker-generated UUID | Identifies one confirmation exchange, not a whole run or tool batch. |
| Tool call `id` | Model/tool protocol | Identifies a call within its batch; it is not a request, run, or confirmation ID. |

Before the first admission, event-envelope `request_id` and `run_id` are empty
strings. Afterwards they identify the active **or most recently admitted** run;
they are not cleared when it finishes. Consequently, a `status` or `error` event
may refer to an already finished run. A rejected input does not replace these
fields: use `input_rejected.data.request_id` to identify the rejected request.

Use `(worker_id, sequence)` for event ordering/deduplication and retain
`session_id` as the session association. A fresh worker may restart its sequence
at 1 for the same session. Sequence numbers do not acknowledge delivery and do
not provide a replay cursor. JavaScript hubs should preserve large 64-bit JSON
integers with a lossless decoder if they may exceed the safe integer range.

## User requests

User requests use `type: "payload"`. They enter a bounded FIFO queue and are
validated when the current run has finished and the consumer dequeues them.
Sending while a run is active does not create a concurrent run.

The read-only `history` payload below is the sole exception: IO routes it
through the control worker, then the application's state executor. It can be
answered while a model call is suspended; it never enters the agent-loop input
queue or changes the conversation.

### Apply options at the next run boundary

Both `message` and `continue` accept an optional `data.options` object. For example:

```json
{
  "type": "payload",
  "data": {
    "operation": "message",
    "request_id": "req-options-1",
    "options": {
      "model": {"model": "deepseek-v4-pro", "reasoning_effort": "max"},
      "tools": {},
      "confirmation": {"mode": "ask"}
    },
    "content": [{"type": "text", "raw": "Explain this carefully."}]
  }
}
```

| Category | Accepted value | Current behavior |
| --- | --- | --- |
| `model` | Object mapping provider option names to values | Passed to the selected provider's synchronous, polymorphic `handle_options()` method. DeepSeek accepts `model`: `deepseek-flash` or `deepseek-v4-pro`, and `reasoning_effort`: `low`, `high`, or `max`. |
| `tools` | Empty object | Reserved; does not change tool configuration. |
| `confirmation` | Object with optional `mode`: `ask`, `approve`, or `deny` | Selects the policy for `RequireConfirm` calls. `ask` is the default and requests the configured confirmation endpoint; `approve` and `deny` decide locally without network IO. |

Omitted categories and omitted option keys retain their current values. An empty
options object is a no-op. Unknown categories, non-object categories, and
nonempty reserved categories are rejected. Providers reject unsupported model
keys or values; `null` is not a reset operation. The default provider handler
supports only an empty object. This API does not expose arbitrary generation
patches, credentials, endpoints, or provider switching.

The worker first validates the whole payload, duplicate request ID, and session
recovery prerequisites. It then validates confirmation options on a temporary
copy, applies model options synchronously, and commits the confirmation selection
before `input_admitted` and before starting the loop. If either category is
invalid, neither selection changes. Invalid options produce `input_rejected`,
preserve the prior settings, and do not consume the request ID or add a user
message. A corrected request may reuse that ID. A queued payload cannot change
the settings of the active run; its options
are processed only after that run settles. All exchanges within the new run use
the selected settings. Signals never apply options: `options` remains a read-only
query, including while a run is active.

Successfully applied settings remain in effect for subsequent runs, including
after cancellation or failure. They are runtime session settings, not part of
the persisted `AgentInputState`; restarting restores the startup model
configuration and confirmation mode `ask`.
A shutdown racing admission can stop the worker after options have been applied
but before a run starts. Applying options is not a delivery or execution guarantee.

### Submit a message

```json
{
  "type": "payload",
  "data": {
    "operation": "message",
    "request_id": "req-001",
    "content": [
      {"type": "text", "raw": "Describe this image."},
      {
        "type": "external_ref",
        "raw": "https://example.com/photo.png",
        "extras": {"detail": "low"}
      }
    ]
  }
}
```

Required fields are string `operation` equal to `message`, a valid `request_id`,
and a nonempty `content` array of objects. Each object becomes one entry in the
user message's ordered `content` list:

| Input part field | Requirement | Conversion |
| --- | --- | --- |
| `type` | Required string: `text`, `binary`, or `external_ref` | `Content.type`; describes encoding, not media category. Unknown values are rejected. |
| `raw` | Required nonempty string | `Content.raw`, unchanged: text, base64 bytes, or an external reference according to `type`. |
| `extras` | Optional JSON object | Additional content metadata, preserved unchanged. Current image options include `detail`; richer modality distinctions will be defined through this object in future extensions. |

For example, the image part above becomes the following persisted/output Content
object. No category field is added during conversion; absent `extras` remains
absent:

```json
{
  "type": "external_ref",
  "raw": "https://example.com/photo.png",
  "extras": {"detail": "low"}
}
```

An attachment-only message is valid; no text part is required. Parts remain in
array order. Unknown extra part fields are ignored; metadata that must survive
conversion belongs in `extras`. `type: "image"` is invalid: use
`type: "external_ref"` with the image URL in `raw`. No `label` parameter is
part of the current input contract.

The worker neither fetches references nor decodes base64 during admission.
There is no upload endpoint or implicit mapping from a hub-local filename to
worker/provider-accessible bytes. The sender must provide the bytes/reference
required by its selected provider adapter. There is no application-level raw
length setting; transport and memory limits still apply. Text is not trimmed.

The current Chat Completions adapter maps `external_ref` to an `image_url`
content part, using `raw` as its URL (including provider-supported image data
URLs). Thus current multimodal input consists of text and image references:

```json
{"type":"image_url","image_url":{"url":"https://example.com/photo.png","detail":"low"}}
```

This is the provider-facing form of the image example above, not a worker input
part. The worker input keeps the provider-independent `type`/`raw` representation.
Image options such as `extras.detail` are forwarded by the adapter. The existing
`extras.image_url` option can override the provider URL; normally omit it and
use `raw` to avoid two competing URLs.

More complex modalities, such as video/audio or binary attachments requiring a
media-specific encoding, will be distinguished through `extras` in future
provider extensions. No category key or values are standardized for those modes
yet. Core can retain binary content and opaque metadata, but that does **not**
make the current adapter support those modalities. Do not send a video reference
as `external_ref` expecting video behavior: this adapter treats it as an image.
Provider adapter support and the selected model's capabilities must both match
what the hub sends.

The former `data.text` field is no longer accepted for `message`, including when
`content` is also present. Send a one-element text array instead. Role and tool
metadata remain worker-owned; a content array is not an arbitrary MessageItem.

### Continue the existing turn

```json
{"type":"payload","data":{"operation":"continue","request_id":"req-002"}}
```

`continue` starts a new invocation on the existing conversation without appending
a user message. It requires at least one existing turn. It is useful after
cancellation or an exchange limit when the recovery phase permits continuation;
it is not restricted to those outcomes. Each invocation receives a fresh model
exchange budget. A `continue` request carrying `content` or the legacy `text`
field is rejected, even if its value is null or an empty array. It never
accepts a new user message.

For both operations, `role`, `invokes`, `invoke_return`, and `type` inside `data`
are rejected even if their values are null. Other unknown fields are ignored.
The forbidden inner `type` is distinct from the required envelope `type`.

### Read a display history page

```json
{"type":"payload","data":{"operation":"history","request_id":"history-1","start":0,"step":0,"limit":10}}
```

`history` is a read-only query, not an agent invocation. It requires a valid
`request_id`; `start` defaults to 0 and is a zero-based turn index; `step`
defaults to 0 and is the first model step within that turn; `limit` defaults
to 10 and must be 1–10 turns. It does not accept `content`, `text`, `options`,
or message metadata fields. It neither consumes the model budget nor emits `input_admitted`.
Invalid queries emit `history_error` with the query ID and a diagnostic.

The `history` event contains `request_id`, `revision`, `start`, `step`, `next`,
`next_step`, `total`, and `turns`. Continue with the returned `next` and
`next_step` cursor until `next == total` and `next_step == 0`.
`revision` increases when the displayed state changes within one
worker instance; it is not persisted and must be scoped by `worker_id`.
Each turn contains its index, up to four ordered user content parts, and the
model steps on this page. A page uses a 256 KiB step budget; a long turn may
therefore span multiple pages, with no fixed step-count cutoff.
Each step contains its index, ordered response content, optional reasoning,
and a tool-call count. `omitted_steps` counts model steps still to be fetched;
`omitted_user_parts` counts input parts beyond the display limit. Each content
list includes at most four parts;
`omitted_parts` on a model step counts its remaining parts.
Tool arguments, results, system prompt, and the rest of `AgentInputState` are
never returned. Text parts are limited to 4096 UTF-8 bytes and external
references to 2048 bytes; `truncated: true` marks clipped values. Binary
contents carry an empty `raw`, `omitted: true`, and their encoded byte length.
This is display data, not a restorable snapshot.

Pages reflect state at the time each query runs. Hooks may prune or edit turns
between pages. If two pages have different `revision` values, discard the
partial result and restart from `start: 0`. Refresh after a run settles.
The response event's `sequence` is the display baseline for subsequent live
events from the same worker. A query can run during an active invocation, but
it observes only records already committed to in-memory state.

### Admission and rejection

The worker emits `input_admitted` after host validation and assigning a run ID.
That event is not proof of input integration, model execution, or persistence.
`run_started` marks loop admission; `input_committed` marks integration of a
new user message into in-memory conversation state. A `continue` request has
no `input_committed` event.

`input_rejected` is emitted for invalid input, a duplicate ID, an unsafe
recovery phase, or continuation without a turn:

```json
{"request_id":"req-001","message":"duplicate request_id in the recent admission window"}
```

This example is the event's `data`. The rejected request ID is echoed as its
original JSON value when present, even if it was not a valid string; otherwise
it is null. Diagnostic messages are human-readable, not stable error codes.
Rejected requests are not inserted into the duplicate cache.

Only the most recent 4096 admitted request IDs are remembered. The cache is
per worker, survives reconnects, and is neither persisted nor shared with another
worker. Evicted IDs and IDs from a previous process can be admitted again. There
is no exactly-once promise. Use fresh request IDs, track outcomes, and do not
blindly replay a request after a disconnect.

## Control signals

Signals use `type: "signal"`. They have a separate queue and handler path so
they do not wait behind the current agent loop's payload consumer. Their actions
are posted onto the worker's state-owning executor. This is cooperative control,
not a hard realtime or interrupt guarantee. Payload processing and signal
processing do not have a combined cross-queue execution order.

| `data.operation` | Required additional fields | Effect and response |
| --- | --- | --- |
| `status` | None | Emits a `status` event containing current state information. |
| `options` | None | Emits an `options` event with available choices and current selections grouped by category; does not change configuration. |
| `cancel` | Nonempty string `run_id` | Requests cancellation only if the ID matches the current/last run ID, then emits `status`. A stale ID changes nothing. |
| `shutdown` | None | Requests process-worker shutdown. No dedicated acknowledgement or final shutdown event exists. |

```json
{"type":"signal","data":{"operation":"status"}}
```

```json
{"type":"signal","data":{"operation":"cancel","run_id":"fd473c9f-f424-4c5a-bcc7-eb39698674de"}}
```

```json
{"type":"signal","data":{"operation":"shutdown"}}
```

Additional signal fields are ignored. No signal request ID is echoed; any
`request_id` in the event envelope still identifies the last admitted payload.
An unknown operation, missing field, or wrong field type emits `error` with a
`message` when the event queue remains usable. A cancel response is a status
snapshot, not a cancellation acknowledgement: `active` may still be true and
loop status may still be `running`. Observe `run_finished` for the settled
outcome when available. Cancelling a finished run does not undo it or affect the
next run. Cancellation does not clear queued payloads; use `shutdown` if no
further inputs should be admitted.

## Worker events

The following table lists every event currently emitted by core. The `data`
column specifies the complete core-defined payload. Optional extension fields
may occur in nested dataclass records.

| Event | `data` | Meaning |
| --- | --- | --- |
| `ready` | Status object | Startup initialization finished and payload consumption is starting. Emitted once per worker lifetime, not once per WebSocket connection. |
| `status` | Status object | Snapshot produced by `status` or `cancel`. |
| `options` | Options object | Available choices and current selections returned in response to the `options` signal. |
| `history` | Display history page | Read-only response to a `history` payload; not a run event. |
| `history_error` | `{ "request_id": any JSON value or null, "message": string }` | Invalid history query. |
| `input_admitted` | `{}` | Host admitted an input and assigned its run ID. |
| `input_rejected` | `{ "request_id": any JSON value or null, "message": string }` | Dequeued input failed host validation; no run was started for that input. |
| `run_started` | `{}` | Loop admitted the invocation. |
| `input_committed` | `{}` | New user input was integrated in memory. |
| `model_response` | Message object | One complete model response was committed in memory. It can contain tool calls and need not be the final answer. |
| `tool_calls` | Array of call objects | Calls proposed for a batch, before dispatch and security evaluation. Not proof of execution or final authorization attributes. |
| `tool_results` | Array of result objects | Complete returned batch was projected into conversation state. Results remain in call order, not completion order. |
| `persisted` | `{ "boundary": string, "format": "json" }` | A required JSON snapshot write completed successfully at the named boundary. |
| `export_error` | `{ "message": string }` | Optional Markdown export failed after successful JSON persistence. |
| `error` | `{ "message": string }`, sometimes also `"durable": false` | Control-validation or worker/storage diagnostic. Not a universal fatal-error notification. |
| `run_finished` | `{ "status": string, "error": string, "exchanges": unsigned integer, "durable": boolean }` | Invocation settled and its configured final persistence was handled. |

All of these are wrapped in the common event envelope. `model_response`,
`tool_calls`, and `tool_results` are separate events, not token or output chunks.
No per-tool-start, per-tool-finish, process-output-stream, or connection-state
event is defined. Fatal startup/transport/queue/storage errors can end the
worker without delivering `error` or `run_finished`; process diagnostics and
connection loss must also be observed by the deployment.

### Options object

The hub can discover available choices without starting a run:

```json
{"type":"signal","data":{"operation":"options"}}
```

The worker replies on the same event connection using its ordinary metadata
envelope. For a DeepSeek worker configured with `deepseek-flash` and `low`, an
example before the first request is:

```json
{
  "type": "event",
  "event": "options",
  "session_id": "demo",
  "worker_id": "204d23ea-0f10-4dbb-b2ef-613bc3f7852d",
  "request_id": "",
  "run_id": "",
  "sequence": 2,
  "data": {
    "model": {
      "available": [
        {"name": "model", "options": ["deepseek-flash", "deepseek-v4-pro"]},
        {"name": "reasoning_effort", "options": ["low", "high", "max"]}
      ],
      "current": {"model": "deepseek-flash", "reasoning_effort": "low"}
    },
    "tools": {"available": [], "current": {}},
    "confirmation": {
      "available": [
        {"name": "mode", "options": ["ask", "approve", "deny"]}
      ],
      "current": {"mode": "ask"}
    }
  }
}
```

| Category | `available` | `current` |
| --- | --- | --- |
| `model` | Provider's `get_options() const` descriptors | Provider's effective runtime values, using the same option names. Providers without advertised choices return `[]` and `{}`. |
| `tools` | `[]` | `{}`; tool configuration is reserved. |
| `confirmation` | One `mode` descriptor with `ask`, `approve`, `deny` | `{"mode":"ask"}` initially; reflects the selected runtime policy. |

Each `available` array contains `{ "name": string, "options": [string, ...] }`
descriptors in display order. `current` contains effective values, not a patch
to apply. A key absent from `current` has no selected value. Startup or trusted
in-process configuration may produce a current value outside the advertised
remote choices. DeepSeek reports its effective `reasoning_effort`, including a
value derived from the startup `reasoning.effort` envelope when no explicit
top-level effort overrides it. Empty tool metadata does not mean tools are
disabled. Hubs should tolerate new categories and provider-defined names and
values. This query exposes no credentials or endpoint configuration and does
not fetch a remote model catalogue.

The query can be handled while idle or while a run is suspended on asynchronous
work. It uses the worker's existing signal path and does not admit an input,
change generation parameters, or create a new run. Metadata identifies the
current or most recently admitted run, exactly as for `status`; the signal has
no separate request ID or echoed correlation ID. Every response gets a new event
sequence number. The Hub can query after reconnecting to reconstruct current
runtime selections. No options event is sent automatically at startup or reconnect.

If provider option discovery throws a standard exception, the worker reports
an `error` event through the normal signal error path and can continue serving
requests. The usual event-queue failure and delivery limits still apply. To apply
runtime choices, include `data.options.model` or `data.options.confirmation` in
the next payload as described under
[Apply options at the next run boundary](#apply-options-at-the-next-run-boundary).

### Status object

```json
{
  "active": false,
  "stopping": false,
  "storage_failed": false,
  "rejected_payloads": 0,
  "loop": {
    "status": "completed",
    "phase": "ready",
    "completed_exchanges": 1,
    "committed_response_sequence": 1,
    "error": "",
    "pending_results": []
  }
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `active` | Boolean | The host currently owns an admitted invocation, including its settlement. |
| `stopping` | Boolean | Worker shutdown is in progress as observed at this snapshot. |
| `storage_failed` | Boolean | A required JSON persistence operation failed; further saves are suppressed. |
| `rejected_payloads` | Nonnegative integer | Cumulative inbound payload-queue overflow count in this IO client lifetime; not semantic input rejections. |
| `capabilities` | Array of strings | Features supported by this worker process. `session-history` means it accepts read-only `history` payloads. A hub should check this before querying a worker that may be older than the hub. |
| `loop` | Optional loop-progress object | Present only when conversation state contains loop progress, including restored progress. |

Loop progress always contains the following fields when present:

| Field | Type / values | Meaning |
| --- | --- | --- |
| `status` | `idle`, `running`, `completed`, `cancelled`, `exchange_limit`, `failed` | Current or most recent loop outcome; restored progress may describe a previous worker. |
| `phase` | `ready`, `model`, `tools`, `projection`, `blocked` | Recovery boundary; see recovery rules below. |
| `completed_exchanges` | Nonnegative integer | Model responses committed during that invocation. |
| `committed_response_sequence` | Unsigned 64-bit integer | Persisted count of model-response commits across invocations, even if hooks later prune history. Unrelated to event `sequence`; not a hub replay cursor. |
| `error` | String | Loop-level diagnostic, empty when none. |
| `pending_results` | Array of result objects | Complete batch awaiting projection; normally empty outside recovery. |

A status response does not include full conversation history, tool definitions,
request-ID history, or the last final-save receipt. It cannot by itself resolve
all delivery or durability ambiguity.

### Run outcome and persistence events

`run_finished.data.status` is one of:

- `completed`: a response without further tool calls was committed.
- `cancelled`: cancellation was observed and required settlement finished.
- `exchange_limit`: the per-invocation model exchange budget was reached after
  settlement of the last batch.
- `failed`: a loop-level error occurred; inspect `error` and the recovery phase.

`exchanges` counts model responses committed in this invocation, not network
attempts, provider retries, tool calls, or lifetime exchanges. Individual tool
failures may be ordinary tool results and need not produce a failed run.

`durable: true` means a successful final/cancellation JSON snapshot was recorded
for this invocation. It does not certify remote event delivery or atomicity of
external tool side effects with disk persistence. `false` also occurs normally
when persistence or final saving is disabled; it does not always indicate an IO
error. Earlier step checkpoints can exist even when this field is false.

`persisted.data.boundary` is one of:

| Boundary | State saved and policy |
| --- | --- |
| `before_tools` | Before dispatch, phase `tools`; mandatory when persistence is enabled. |
| `results_ready` | Returned results buffered, phase `projection`, before projection; mandatory when persistence is enabled. |
| `step_finished` | After tool-result projection and validated step edits, if `on_step_finished` is enabled. |
| `run_finished` | Final invocation state, if `on_run_finished` is enabled. |
| `cancelled` | Settled cancellation state when final saving is disabled; still saved if persistence is enabled. |
| `shutdown` | State during controlled shutdown, if `on_shutdown` is enabled. |

No `persisted` event is emitted when persistence is disabled. There is no
Markdown-success event. A successful JSON write can be followed by
`export_error` without invalidating the JSON snapshot. Event sequence numbers
and pending event queues are not part of the persisted conversation.

## Shared data types

These shapes are embedded directly in event `data`, confirmation `data.call`,
and loop progress. Optional fields are omitted when absent. `extras` is optional
opaque JSON, not necessarily an object except for the recognized result markers
below. Hubs should retain unknown provider/plugin fields without interpreting
them as commands.

### Content

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | `text`, `binary`, `external_ref` | Encoding of `raw`. |
| `raw` | String | UTF-8 text, base64 binary, or an external URI/reference, respectively. |
| `extras` | Optional JSON | Provider/tool metadata. Admitted user content retains the supplied metadata object unchanged. |

Render text as untrusted content. Do not execute HTML, terminal escape sequences,
or references received from models or tools. URI references are data; the
protocol does not require automatically fetching them.

### Message object

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | `user_input`, `model_response`, `invoke_return` | Kind of conversation record; normal `model_response` events contain `model_response`. |
| `role` | String | Model/conversation role, normally `assistant` for model responses. |
| `content` | Array of Content | Ordered content parts, possibly empty. |
| `reasoning` | Optional Content | Provider-supplied reasoning content. |
| `action_status` | Optional Content | Provider-supplied action-status content. |
| `invokes` | Optional array of calls | Proposed tool calls. |
| `cost` | Optional token-cost object | Provider-reported usage; absence means unavailable, not zero usage. |
| `invoke_return` | Optional result object | Provenance for a tool-result message. |
| `extras` | Optional JSON | Additional provider metadata. |

A token-cost object has nonnegative integer fields `prompt`, `generated`, and
`cache_hit`. `cache_hit` counts tokens within `prompt`; do not add it again when
computing total tokens. Usage is provider-reported and may be partial.

```json
{
  "type": "model_response",
  "role": "assistant",
  "content": [{"type":"text","raw":"Hello."}],
  "cost": {"prompt":120,"generated":4,"cache_hit":80}
}
```

### Call object

| Field | Type | Meaning |
| --- | --- | --- |
| `id` | String | Tool call identity. Use this and batch context, not a display label, to correlate results. |
| `name` | String | Tool name. |
| `arguments` | JSON, normally an object | Tool-specific arguments. |
| `type` | `read_only`, `parall_write`, `serial_write` | Scheduling classification. `parall_write` is the exact wire spelling. |
| `security` | `default_deny`, `require_confirm`, `trusted` | Security classification. |
| `extras` | Optional JSON | Call metadata. |

Calls in a model response or `tool_calls` event have not necessarily been
normalized or classified by the tool registry. Confirmation requests carry the
settled arguments and host-resolved classification. Result queries preserve
the settled call when execution reached that stage. A hub must not authorize
from a proposed call's `security` field or assume all three representations are
byte-identical.

### Result object

| Field | Type | Meaning |
| --- | --- | --- |
| `query` | Call object | Call this result answers. |
| `output` | Content | Human/model-readable result. |
| `extras` | Optional JSON | Machine-readable result annotations. |

Recognized core/tool-framework annotations, when `extras` is an object:

- `error`: object with string `stage` and `message`. Stages are `dispatch`,
  `argument_parse`, `security_check`, `invoke`, `result_check`, or `unknown`.
  A denial normally appears as `security_check`. Diagnostic text is not a stable
  machine code. An error at invocation/result validation does not imply that
  no side effect occurred.
- `cause_query`: a call object preserving a different underlying call involved
  in an error or result correlation. `query` remains the call being answered.
- `loop_skipped: true`: the loop did not dispatch this call, for example because
  cancellation was already observed. It is a non-execution result, not tool output.

Other annotations are tool-specific. There is no universal success boolean or
process-result JSON schema in the worker envelope. The intrinsic process tools
currently render human-readable text, including separate stdout/stderr sections;
these are not separate transport streams. A hub can display `output.raw` without
parsing its prose. Tool/plugin-specific structured output needs that tool's own
contract, not assumptions based on its name.

## Tool confirmation

### Run policy

`options.confirmation.mode` in a payload selects how the worker answers
`RequireConfirm` requests for that run and subsequent runs:

- `ask` (default): request the configured confirmation endpoint. A missing
  endpoint denies the call, preserving the startup behavior.
- `approve`: approve locally without opening a confirmation connection.
- `deny`: deny locally without opening a confirmation connection.

The policy is frozen in the run's confirmation scope before `run_started` and
shared by every confirmation in that run, including parallel tool calls.
Queued payloads and read-only `options` signals cannot alter it. Automatic
approval still arbitrates against run cancellation using the same synchronized
boundary as endpoint approval; a missing or cancelled scope never approves.
`Trusted` calls still pass and `DefaultDeny` calls still fail without consulting
this policy. Individual tools may also implement their own security checks.
The deployment trust requirement for `approve` is specified under
[Connections and configuration](#connections-and-configuration).

Only `mode` is remotely configurable. Unknown keys, unsupported modes, and
non-string modes are rejected without changing either model or confirmation
settings. Endpoint and timeout remain startup configuration. `{}` preserves the
current mode; `null` is invalid. Settings are not persisted with conversation
state, and a restarted worker begins in `ask` mode.

### Request

In `ask` mode, for each call requiring confirmation, the worker opens the configured confirmation
WebSocket, sends one text message, and awaits one response:

```json
{
  "type": "confirmation_request",
  "data": {
    "worker_id": "204d23ea-0f10-4dbb-b2ef-613bc3f7852d",
    "session_id": "demo",
    "run_id": "fd473c9f-f424-4c5a-bcc7-eb39698674de",
    "confirmation_id": "573e354f-7c5d-4ca6-bf34-72b4a68597b2",
    "call": {
      "type": "serial_write",
      "security": "require_confirm",
      "id": "call-001",
      "name": "run_command",
      "arguments": {"command":"printf 'hello\\n'"}
    }
  }
}
```

The call shape is complete; arguments in this illustrative example are
abbreviated relative to tool-specific normalization. Show the actual settled
arguments received, rather than an earlier model proposal. The worker's
confirmation request has no `request_id` or event `sequence`.
Correlate by `(worker_id, session_id, run_id, confirmation_id)` plus the server's
authenticated connection/deployment association. `worker_id` matches the event
connection for this worker instance but is not an authentication token. A
confirmation can arrive before `tool_calls` or `run_started` is delivered over
the independent event connection.

`trusted` calls need no hub approval. `default_deny` is not made trusted by a
hub-supplied decision. Do not expect a confirmation for every proposed call.

### Response

The hub sends exactly one text response on the same confirmation connection:

```json
{
  "type": "confirmation_response",
  "data": {
    "worker_id": "204d23ea-0f10-4dbb-b2ef-613bc3f7852d",
    "session_id": "demo",
    "run_id": "fd473c9f-f424-4c5a-bcc7-eb39698674de",
    "confirmation_id": "573e354f-7c5d-4ca6-bf34-72b4a68597b2",
    "decision": "approved",
    "reason": "Approved by the operator"
  }
}
```

All four IDs must exactly match. A missing or mismatched `worker_id` denies the
call. `decision` must be `approved` or `denied`.
`reason` is optional, must be a string if present, and defaults to
`operator decision`. Unknown additional fields are ignored. Do not wrap this
response as an event-connection signal, and do not send it on the event socket.

After reading the response, the worker initiates a normal WebSocket close
handshake. The hub must participate promptly: the exchange, including closing,
must complete within the confirmation deadline before the decision can be
accepted. There is no separate approval-accepted acknowledgement. A hub that
sends approval cannot infer tool execution until it observes subsequent results.

### Deadline, disconnection, and cancellation

There are no retries for a confirmation exchange. The default 120000 ms deadline
covers DNS, TCP, TLS, upgrade, request write, response read, and graceful close.
In `ask` mode, a missing endpoint, invalid JSON, wrong envelope, mismatched ID, unknown decision,
wrong reason type, binary response, disconnection, timeout, or cancellation
results in denial. A timed-out/closed request must not be retried with the same
approval on a new connection.

Run cancellation closes confirmation admission before requesting loop
cancellation. Approval and cancellation arbitrate under synchronization: approval
that wins is allowed into the batch; cancellation that wins prevents approval.
Cancellation does not revoke an already accepted approval or roll back its side
effects. Multiple batch confirmations may therefore settle differently.

There is no explicit confirmation-cancel message. The worker aborts pending
confirmation connections. Hubs must retire prompts when those connections close
and must not apply an answer to a later request. An event-connection disconnect
alone is not a cancellation: the worker can continue running and its separate
confirmation connection may still be valid.

A deadline is an authorization cutoff, not a hard bound on coroutine completion.
A DNS backend already inside a system resolver may delay final cleanup. Its late
result cannot revive an expired approval. Numeric endpoint addresses avoid the
DNS lookup when this latency matters.

## Ordering and example exchanges

On a healthy event connection, events are written in their generation order.
For a new message that completes without tools, the normal sequence with final
saving enabled is:

```text
hub -> worker: payload(message, request_id=req-001)
worker -> hub: input_admitted       request_id=req-001, new run_id
worker -> hub: run_started
worker -> hub: input_committed
worker -> hub: model_response       complete assistant message
worker -> hub: persisted            boundary=run_finished
worker -> hub: run_finished         status=completed, durable=true
```

A normal tool-bearing exchange, inside a run, has this ordering:

```text
event connection: model_response (contains invokes)
event connection: tool_calls
event connection: persisted (before_tools)
confirmation connection(s): request -> response -> close, if required
event connection: persisted (results_ready)
event connection: tool_results
event connection: persisted (step_finished), if configured
next model exchange, or final persistence and run_finished
```

The ordering above describes worker-side lifecycle stages. It is **not** a
cross-connection arrival guarantee for confirmations. Process each connection
independently. Denied or skipped calls can still produce `tool_results`.

These are normal examples, not mandatory event counts. Early cancellation can
produce an admitted input without `run_started` or `input_committed`. Hook,
persistence, projection, or transport failure can interrupt the sequence.
`model_response` alone is not a completion signal. A no-tool exchange does not
emit `tool_results` or `step_finished` persistence. Optional Markdown failures
can interleave `export_error` after `persisted`.

## Delivery, backpressure, and reconnects

The stages of a request/result have different meanings:

```text
hub sends -> inbound queue -> input_admitted -> input_committed (new message)
          -> loop/model/tool work -> persistence (when configured) -> run_finished
```

A successful WebSocket send at the hub does not prove queue admission, execution,
or durability. An event admitted to the worker's outbound queue does not prove
receipt by the hub. The protocol has no event ACK, input receipt ACK before
admission, durable outbox, replay request, or exactly-once execution mechanism.

| Queue / failure | Current behavior |
| --- | --- |
| Payload queue full | Incoming payload is dropped and `rejected_payloads` increases. There is no per-request `input_rejected` event for this case. |
| Signal queue full | Fatal IO error; worker shutdown is initiated. |
| Worker event queue full | Fatal worker error, cancellation and cleanup; results are not silently discarded as if delivery succeeded. |
| Transport write queue full | Sender waits; pressure can eventually fill the worker event queue. |
| Invalid JSON, malformed envelope, unknown envelope `type`, or binary input | Fatal event-client error; no automatic reconnect for that application/protocol failure. |
| Valid envelope with invalid payload | `input_rejected`, when dequeued and the event path is usable. |
| Valid envelope with invalid signal | `error`, when processed and the event path is usable. |

Queue capacities count messages, not bytes. There is no public configurable
application-message byte limit; transport library limits and available memory
still constrain messages. Hubs should avoid sending a burst without observing
admission and should keep reading events while waiting for user decisions.

Connection establishment failures retry indefinitely, including permanent DNS,
certificate validation, and HTTP upgrade rejections such as 401/403. Established
connection IO failures and peer closes also reconnect unless stopping. Backoff
starts at 250 ms, doubles up to 10000 ms by default, and resets after a connection
lasts at least 30 seconds. There is no jitter or configured attempt limit. This
policy allows endpoint repair while the process remains alive; a bad endpoint
can leave the worker running without ever becoming connected.

Undequeued outbound messages survive reconnects in memory. Once the writer has
removed a message, it is never automatically replayed: a failed write has an
unknown delivery outcome. The hub may therefore observe sequence gaps.
Stopping discards undelivered messages. Inbound payloads already queued and
in-progress loop work are not cancelled merely because the event socket drops.
Prolonged disconnection can eventually cause fatal event-queue pressure.

`ready` is generated once at startup, possibly before the first successful
connection, and is not regenerated on reconnect. The hub should send `status`
after a new upgrade rather than wait indefinitely for `ready`. Any received
event carries the current worker/session identity; there is no separate
registration message. Reconnects may first deliver old queued events, so do not
treat the first event's status as a fresh handshake snapshot.

On disconnect, retain uncertain request outcomes. A later status can show the
last admitted request/run and current progress, but cannot answer arbitrary
historical request queries or prove whether a lost request was never admitted.
Resolve ambiguity using deployment-owned persisted state or operator inspection,
not automatic resubmission. Use the current worker identity when interpreting
responses from a restarted session.

## Cancellation, shutdown, and recovery

Cancellation targets an invocation. Model work can be interrupted; a tool batch
already admitted is allowed to settle and its complete results are committed.
This does not promise cancellation of arbitrary tool side effects or a bounded
shutdown duration. Calls not dispatched can be recorded as `loop_skipped`.
Pending confirmations are denied when cancellation wins. The host does not
admit the next queued payload until settlement completes.

Shutdown targets the worker. It prevents further admission, cancels active work,
attempts configured persistence, and cleans up owned process sessions before
joining IO tasks. Owned child processes are terminated/reaped as far as possible;
remaining process pipes are closed after the cleanup drain allowance, and
captured output is retained with truncation indicated where necessary. It does
not promise termination of every descendant process. Cleanup errors are reported
locally after best-effort cleanup, not as a guaranteed final wire event.
The final event-queue-to-transport admission attempt is bounded by 500 ms; it is
not a peer-delivery deadline. The event connection may simply close.

Persistence is local to the worker. JSON snapshots reside at
`<persistence.directory>/<session_id>/state.json`; optional readable Markdown is
`readable.md` in the same directory. There is no protocol operation to download,
replace, edit, or reset a snapshot. A hub needing those capabilities must provide
a separate managed integration, not send invented signal operations.

A persistent worker takes an exclusive local session lock before restoration.
Another worker using the same session directory fails startup. This protects
cooperating workers on the same supported local filesystem, not workers using
independent disks or a distributed hub's global session namespace.

Recovery behavior is controlled by `loop.phase`, independently of `loop.status`:

| Phase | Meaning and permitted recovery |
| --- | --- |
| `ready` | No model exchange or tool batch is pending. A valid message/continuation can run. |
| `model` | Model request was in progress without a committed response. New work may resume without replaying tool execution from this phase. |
| `tools` | Dispatch may have produced side effects without saved results. New inputs are rejected pending operator inspection. |
| `projection` | A complete returned batch is buffered. The next invocation first projects those results without redispatching tools. |
| `blocked` | Tool execution failed with uncertain effects. New inputs are rejected pending operator inspection. |

Projection recovery does not regenerate the old batch's `tool_calls` or
`tool_results` events. Hubs must not expect a historical event replay.

Restoration/reconnect never starts a loop automatically. The hub must submit an
explicit new message or `continue` when recovery permits it. Operator-required
states cannot be cleared by a wire operation. Runtime process handles,
confirmation approvals, request deduplication, and event counters are not
restored. Historical process IDs are opaque and cannot address new process
handles after restart.

A required JSON save failure stops further admission and latches storage failure;
later saves do not overwrite the last recovery evidence. A failure after file
rename can leave a visible new snapshot with uncertain crash durability.
Persistence receipts cannot eliminate the crash window between an external tool
side effect and its saved result. Treat `tools`/`blocked` conservatively.

## Hub implementation requirements

A conforming hub integration should:

1. Accept worker-initiated text WebSockets on the configured routes and implement
   standard ping/pong and close handling. Do not require an unimplemented auth,
   registration, subprotocol, or application-heartbeat message.
2. Associate connections with a deployment-authorized worker/session, retaining
   worker IDs across reconnects and distinguishing process restarts. Authorize
   payload senders as approval authorities when `confirmation.mode: approve`
   is available; do not expose the worker-facing socket to untrusted clients.
3. Generate valid request IDs, wait for admission/outcome events, and preserve
   unknown outcomes instead of retrying side-effecting work automatically.
4. Decode every event shape above, including empty objects and array payloads;
   treat text as untrusted and tolerate unknown extension fields/events.
5. Service confirmation sockets concurrently with the event stream, validate all
   correlation IDs, complete the close handshake, and retire disconnected prompts.
6. Target cancellation by run ID and wait for settlement when available. Do not
   assume cancel or disconnection rolls back accepted tool calls.
7. Keep event reads flowing, track sequence gaps and overflow counters, and use
   status requests after reconnect without expecting another `ready` event.
8. Surface run failure, storage failure, recovery-required phases, and connection
   loss distinctly. Displaying a final model message is not proof of a durable
   successful run.

These rules apply equally to a one-to-one terminal server and a multi-worker hub.
A hub's own browser/API protocol may differ, but its worker-facing adapter must
preserve the distinctions documented here.
