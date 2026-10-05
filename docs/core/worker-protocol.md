# Simplex Loop Worker Protocol

This is the communication contract implemented by `simplex_worker`. It is
intended for authors of hubs, gateways, terminal clients, and web interfaces in
any language. The deprecated `simplex_shell` source is a historical example, not a required
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
- [Remote tool requests](#remote-tool-requests)
- [Ordering and example exchanges](#ordering-and-example-exchanges)
- [Delivery, backpressure, and reconnects](#delivery-backpressure-and-reconnects)
- [Cancellation, shutdown, and recovery](#cancellation-shutdown-and-recovery)
- [Hub implementation requirements](#hub-implementation-requirements)
- [Bundled Hub implementation](#bundled-hub-implementation)

## Connections and configuration

The worker uses a persistent event connection and optional one-shot connections
for confirmation and remote tools:

| Role | Worker configuration | Lifetime | Traffic |
| --- | --- | --- | --- |
| Events and inputs | `client.endpoint` | Persistent, reconnecting | Hub sends payloads and signals; worker sends events. |
| Tool confirmation | `security.confirmation.endpoint` | One connection per confirmation | Worker sends one request; hub sends one response; worker closes. |
| Remote tools | `hub_remote_call.endpoint` | One connection per tool request | Worker appends an operation route and exchanges one request/response; see [Remote tool requests](#remote-tool-requests). |

All are WebSocket endpoints, not ordinary HTTP POST handlers. They may use
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
  system_prompt_file: prompts/coding_agent.yaml
persistence:
  enabled: true
  directory: ./data/session
  state: state
  memory: memory
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
configuration denies calls that require confirmation while the policy is `ask`;
payload-selected `approve` and `deny` policies decide locally. Queue capacities,
backoff delays, confirmation timeout, and `max_exchanges` must be positive.
`max_backoff_ms` must be at least `initial_backoff_ms`. The idle timeout is
nonnegative; zero disables it.

`worker.system_prompt_file` selects an independent YAML prompt file for new
sessions. It is a relative path below the executable's directory — the
installation directory — and never resolves against the configuration file, so
a session's generated configuration can live anywhere and still name the
deployed prompt. Empty values, `..` components, and every rooted spelling are
rejected; "rooted" covers both path grammars (a leading `/` or `\`, and a drive
letter with or without a separator), because the file may be written on one
platform and read on another. Containment is lexical, not a sandbox: symlinks
are not resolved, so a link below the installation directory points wherever it
points.
Omitting it reads `prompts/coding_agent.yaml` beside the executable. The file is
validated at startup, including when restoring a session; missing or malformed
files fail startup. Restored sessions retain their stored prompt. Current tool
skills and configured `worker.environment` hints are refreshed in both cases.
Environment hints describe the workspace, platform, and expected software without
changing the working directory or restricting access. See
[environment configuration](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/load/README.md#runtime-environment-hints) and the [prompt file format](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/load/README.md#system-prompt-files).
The previous inline `worker.system_prompt` field is rejected.
`worker.compact_prompt_file` names the internal compact instruction and follows
the same rule; omitting it reads `prompts/operations/compact.yaml` beside the
executable, and its rendered text must not be empty.

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
{"type":"payload","data":{"operation":"message","request_id":"req-001","content":[{"type":"text","raw":"Hello","modality":"text"}]}}
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
| `request_id` | Hub | Nonempty string of at most 128 UTF-8 bytes. Identifies an admitted message, continuation, or compaction within the worker's bounded duplicate window; for `history`, it only correlates a read-only response and is not cached. |
| `run_id` | Worker-generated UUID | New for each admitted `message`, `continue`, or `compact` invocation. Read-only `history` queries do not create runs. Required to target cancellation. |
| `sequence` | Worker | Unsigned 64-bit event counter, starting at 1 and increasing across reconnects within this worker instance. |
| `confirmation_id` | Worker-generated UUID | Identifies one confirmation exchange, not a whole run or tool batch. |
| Tool call `id` | Model/tool protocol | Identifies a call within its batch; it is not a request, run, or confirmation ID. |

Before the first admission, event-envelope `request_id` and `run_id` are empty
strings. Afterwards they identify the active **or most recently admitted** run;
they are not cleared when it finishes. Consequently, a `status` or `error` event
may refer to an already finished run. `input_rejected` is an exception: its
envelope names the rejected string request ID (or an empty string for a
non-string ID), and its `run_id` is empty. The worker's current run identity
remains unchanged. Use `input_rejected.data.request_id` as the authoritative
rejected value, including invalid or missing IDs.

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

`message`, `continue`, and `compact` accept an optional `data.options` object. For example:

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
    "content": [{"type": "text", "raw": "Explain this carefully.", "modality": "text"}]
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
invalid, neither selection changes. Invalid options produce `input_rejected`
with `code: "invalid_options"`,
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
      {"type": "text", "raw": "Describe this image.", "modality": "text"},
      {
        "type": "external_ref",
        "raw": "https://example.com/photo.png",
        "modality": "image",
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
| `type` | Required string: `text`, `binary`, or `external_ref` | `Content.type`; describes **encoding only**, never the media category. Unknown values are rejected. |
| `raw` | Required nonempty string | `Content.raw`, unchanged: text, base64 bytes, or an external reference according to `type`. |
| `modality` | Required string: `text`, `image`, `audio`, `video`, or `document` | `Content.modality`; the media category of the payload. Unknown values are rejected; the field is never derived from `type`. |
| `extras` | Optional JSON object | Additional content metadata, preserved unchanged. Image options include `detail`; provider-specific part fields belong here. |

For example, the image part above becomes the following persisted/output Content
object. `modality` is carried through as sent; absent `extras` remains absent:

```json
{
  "type": "external_ref",
  "raw": "https://example.com/photo.png",
  "modality": "image",
  "extras": {"detail": "low"}
}
```

An attachment-only message is valid; no text part is required. Parts remain in
array order. Unknown extra part fields are ignored; metadata that must survive
conversion belongs in `extras`. `type: "image"` is invalid: media categories are
labels, not encodings, so an image reference is
`{"type": "external_ref", "modality": "image"}`.

`type` and `modality` are independent and both explicit. Sending an image as
`binary` (a base64 payload) or as `external_ref` (a URL) is the sender's choice
of encoding; saying `modality: "image"` is how the worker knows it is an image.
The encoding never implies the category, so a reference to a PDF is
`modality: "document"` and is not sent to a provider as a picture, and a text
part whose bytes ride in a reference stays `modality: "text"`.

The worker neither fetches references nor decodes base64 during admission.
There is no upload endpoint or implicit mapping from a hub-local filename to
worker/provider-accessible bytes. The sender must provide the bytes/reference
required by its selected provider adapter. There is no application-level raw
length setting; transport and memory limits still apply. Text is not trimmed.

### What an adapter does with a modality

Core admits all five categories — what the worker may carry is the contract's
business — and the selected provider adapter maps the ones it can describe. An
adapter never silently converts an unsupported category into text or into a
different category; it fails request construction instead, so a mismatch is
reported rather than sent as a corrupted prompt.

Support is a property of the **pair**, not of the category alone: a kind is only
usable when the field it maps to can carry the representation `type` declares.

| Adapter | Encoding | Modality | Provider part |
| --- | --- | --- | --- |
| Chat Completions | `text` | `text` | `text` |
| Chat Completions | `external_ref` | `text` | `text` (the reference travels as text; it is not fetched) |
| Chat Completions | `external_ref` | `image` | `image_url` |
| Responses | `text` | `text` | `input_text` |
| Responses | `external_ref` | `text` | `input_text` |
| Responses | `external_ref` | `image` | `input_image` |
| Responses | `external_ref` | `document` | `input_file` with `file_url` |
| Responses | `binary` | `document` | `input_file` with `file_data` |

Every other combination is rejected at request construction, including
`binary` with `text` or `image` (a base64 blob is neither a message nor an
image URL, and nothing in the part carries the media type a data URL would
need) and `text` with `document` (a plain string is not file data). The
matrices are deliberately narrow; each can grow when a representation gains the
metadata it needs.

The matrix belongs to the **position** a part is emitted at, not to the
conversation as a whole:

| Position | What the adapter can carry |
| --- | --- |
| User input | the full table above |
| Tool result | Chat Completions: text only (its tool output is one string). Responses: the full table above (tool output is an input-list array) |
| Assistant replay | text only in both adapters — an assistant message is replayed as text (a Chat Completions `content` string, a Responses `output_text` part) |
| Reasoning | text only, and only where it is replayed at all (Chat Completions when the dialect opts in; Responses when it is synthesized rather than re-emitted from captured items) |
| `action_status` | mapped by neither adapter, so no provider capability applies and it is not validated |

An image is therefore legal in a user message and impossible in an assistant
message: replaying it there would send the characters of its URL as if the model
had written them. A part the adapter never emits — `action_status` today, or
anything replayed verbatim from provider-captured metadata — is not a provider
capability question and does not fail construction.

`extras` is sender-supplied **data, never an instruction about the part kind**.
An adapter builds the provider part from `modality` and the fields that kind
defines (`detail`, `filename`, `file_id`, `image_url`); a `type` inside `extras`
cannot relabel a text part as a file or a document as an image, and fields the
chosen kind does not define are not forwarded.

An image part mapped by the Chat Completions adapter uses `raw` as its URL,
including provider-supported image data URLs:

```json
{"type":"image_url","image_url":{"url":"https://example.com/photo.png","detail":"low"}}
```

This is the provider-facing form of the image example above, not a worker input
part. The worker input keeps the provider-independent
`type`/`raw`/`modality` representation. Image options such as `extras.detail` are
forwarded by the adapter. The existing `extras.image_url` option can override the
provider URL; normally omit it and use `raw` to avoid two competing URLs.

Audio and video are contract labels without a provider mapping today: the worker
retains them and the panel can display them, but both current adapters reject
them at request construction rather than guessing a category. Document has a
Responses mapping and no Chat Completions mapping; a provider-hosted file is
selected with `modality: "document"` plus `extras.file_id`. Do not send a video
reference expecting video behavior. Provider adapter support and the selected
model's capabilities must both match what the hub sends.

Sessions persisted before `modality` existed are read with the semantics those
records were written under: a stored `external_ref` becomes `image`, which is
what both adapters used to send it as, and a stored `text` becomes `text`. A
stored `binary` part without a label is **refused** with a migration error,
because the two adapters disagreed about it (Chat Completions sent the base64 as
message text, Responses as file data) and no reading is faithful to both. The
next save rewrites the recovered label, so the migration happens once. This
tolerance applies to durable records only: the input boundary above always
requires the field.

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
model steps on this page. The complete compact UTF-8 JSON data object, including
user content, response content, reasoning, paging metadata, `revision`, separators
and JSON escaping, is limited to **252 KiB (258048 bytes)**. The worker reserves
another **4 KiB** for its event envelope, giving a **256 KiB (262144 bytes)**
complete history-event limit. These limits exclude WebSocket frame headers and
apply to the default unindented JSON encoding, not pretty-printed diagnostics.
The worker checks its final serialized event before queueing it.

Turns and model steps are admitted whole; a long turn may span multiple pages,
with no fixed step-count cutoff. A nonterminal page always advances the
`next`/`next_step` cursor. The user projection is repeated on continuation pages
of the same turn; merge that turn's steps by their indices rather than adding
another user entry. A turn with no model steps still consumes the byte budget
and advances `next`. Requesting `step` equal to the turn's step count returns
that turn's user projection and advances to the next turn.

The current four-part and per-part limits ensure that one user projection plus
its first remaining model step fits an empty page, even when every raw byte
requires a six-byte JSON escape. Pagination adds no further text truncation.
If a future projection cannot fit such an indivisible entry, the query emits
`history_error` rather than exceeding the limit, silently omitting content, or
returning a cursor that cannot advance. Invalid UTF-8 or other projection errors
also remain query failures; they do not start an agent run.
Each step contains its index, ordered response content, optional reasoning,
and a tool-call count. `omitted_steps` counts model steps still to be fetched;
`omitted_user_parts` counts input parts beyond the display limit. Each content
list includes at most four parts;
`omitted_parts` on a model step counts its remaining parts.
Tool arguments, results, system prompt, and the rest of `AgentInputState` are
never returned. Text parts are limited to 4096 UTF-8 bytes and parts of any other
modality to 2048 bytes; `truncated: true` marks clipped values. Binary
contents carry an empty `raw`, `omitted: true`, and their encoded byte length.
Each projected part carries `type` and `modality`.
This is display data, not a restorable snapshot.

The built-in Hub preserves both the parsed event and its original `raw` document.
Its complete panel `event` message therefore fits within **512 KiB (524288
bytes)** for these worker history pages, including the duplicated data,
bounded built-in identifiers, Hub metadata and panel wrapper. This is a derived
upper bound, not a separate Hub admission setting. Arbitrary older/custom worker events remain
subject to the Hub's general transport and output limits; this worker projection
does not impose a new limit on their protocol. Existing `session-history`
capability and cursor fields are unchanged, so compatible clients need no new
capability negotiation.

Pages reflect state at the time each query runs. Hooks may prune or edit turns
between pages. If two pages have different `revision` values, discard the
partial result and restart from `start: 0`. Refresh after a run settles.
The response event's `sequence` is the display baseline for subsequent live
events from the same worker. A query can run during an active invocation, but
it observes only records already committed to in-memory state.

### Compact conversation context

A worker advertising `context-compact` accepts:

```json
{"type":"payload","data":{"operation":"compact","request_id":"compact-001"}}
```

`compact` accepts the same optional `options` as other run requests, applied at
admission. It must not carry `content` or `text`. It requires enabled persistence,
at least one conversation turn, and a settled `ready` phase (or no loop progress).
`tools`, `blocked`, `model`, and `projection` require resolution before compact.
Invalid requests receive `input_rejected`. A request accepted into the payload
queue waits for earlier runs to settle, just like `message` and `continue`.

Before model execution, the worker exports the original AgentInputState to a new
readable Markdown archive under `<persistence.directory>/<persistence.memory>`
(default memory subdirectory `memory`). Each exclusively created archive directory
is `<20-digit ordinal>-<UTC timestamp>-<run_id>/`, containing `state.md`. Ordinals are derived from existing archives and increase across worker restarts
and clock changes. Old directories and files are never reused. Failed or cancelled attempts retain their archives;
export failure can leave an empty directory. Readable exports include all history,
with the existing JSON-preview clipping and binary omission policy. They are not
lossless restorable snapshots. Export failure prevents the model request.

The worker appends its startup-loaded compact instruction to a private state copy
and runs one model exchange without tools. Extension hooks, temporary input,
model-response events, and automatic saves are excluded from this private run.
Tool calls returned despite the instruction are rejected before dispatch. Empty
or whitespace-only text summaries fail; reasoning and non-text content are not
injected into memory. While summarization is pending, history queries continue
to return the original conversation, and the live state remains unchanged. The
private run uses the built-in context statistic hook to account for its response before pruning; successful publication
retains that accounting and refreshes the estimate for the new system prompt.
`run_started` follows the successful archive write.

The worker rejects an empty summary, one above 32 KiB, or a replacement that
does not reduce the measured context by at least 10%. The replacement must also
fit a byte budget: the lesser of 64 KiB and 75% of the configured context window
token count. This deterministic measure adds the rendered system prompt and
JSON-encoded tools and turns, in UTF-8 bytes. It is a conservative size proxy;
provider-specific tokenization and message wrappers can differ. These checks
run before the mandatory JSON save. A rejected result leaves the original state
authoritative and emits no `compact_finished`.

Only a successful, non-cancelled summary replaces the live state. All user turns
are removed; other state fields are retained, apart from the updated timestamp,
completed loop progress, built-in context statistics, and replacement
`memory.runtime` prompt section. The response commit sequence continues monotonically. This last Volatile section
contains the summary, the absolute session archive directory, and the latest
archive file path, with instructions to read archives for historical details.
The summary sits between a unique begin/end marker and follows a fixed warning
that it is untrusted historical context, not system policy or current user intent.
It replaces older injected memory; archive retention is described below. Restoring
a worker preserves memory after the refreshed runtime signature.

The new JSON snapshot is mandatory even when `save.on_run_finished` is false.
It is atomically saved before the in-memory replacement and success notification.
Cancellation before that commit preserves the original state. Cancellation during
the synchronous commit does not undo it. Required JSON save failure stops further
admission; a failure after file publication has uncertain durability and is never
reported as success. Optional `readable.md` export failure emits `export_error`
without undoing successful JSON publication.

On success the worker emits `persisted` with boundary `compact`, then
`compact_finished`, then `run_finished` with `status: completed` and `durable: true`.
`compact_finished.data` contains:

```json
{"summary":"Summary text", "memory_file":"/absolute/archive/state.md", "removed_turns":12, "revision":38, "durable":true}
```

The history revision advances once at replacement. Clients should invalidate old
history pages and display the summary as a compact result, not a new user turn.
A failure or cancellation emits `run_finished` without `compact_finished` and
without changing the authoritative conversation or loop progress; its status
object can therefore still describe the preceding ordinary run. `durable: false`
for this attempt does not invalidate the original snapshot. Retrying compact uses
a fresh request ID. There is no automatic retry, and a failed or cancelled
attempt does not trigger archive cleanup; a later successful compact may remove
its archive under the retention policy below.

After success, history contains zero turns. `continue` is rejected until a new
message creates a turn; that message sees the new system-prompt memory.
The hub exposes this operation as **Compact context** in the composer's Command
mode. Both hub and current worker advertise `context-compact`. The panel retires
outstanding history queries, refreshes the new revision, and renders the summary
as a compact result. Retained hub events remain available as an execution record.

After each successful state replacement the worker applies
`persistence.memory_retention.max_archives` (default 5). Zero disables cleanup.
It always keeps the current archive, which counts toward the limit, then
retains other recognized archives newest first until the count limit is reached.
Cleanup is synchronous under
session ownership, after the durable commit, with no archive writer or tool
running. Failed or cancelled attempts remain until a later successful compact;
these limits are therefore cleanup targets, not a hard disk quota.

Only ordinary directories matching the worker archive name and containing
exactly one regular `state.md` are eligible. Symlinks, extra files and empty or
unrecognized directories are left untouched. Deletion is never recursive. This
assumes cooperating filesystem users, not hostile concurrent path replacement.
The status object reports the active policy. `compact_finished` additionally
reports `archive_cleanup: {removed_archives, removed_bytes}` or
`archive_cleanup_error: string`; a cleanup failure does not undo a saved summary.
The panel displays cleanup failures. The worker owns cleanup even when its files
are remote to the hub; the hub never deletes a path received in an event.

### Admission and rejection

The worker emits `input_admitted` after host validation and assigning a run ID.
That event is not proof of input integration, model execution, or persistence.
`run_started` marks loop admission; `input_committed` marks integration of a
new user message into in-memory conversation state. A `continue` request has
no `input_committed` event.

`input_rejected` is emitted for invalid input, a duplicate ID, an unsafe
recovery phase, continuation without a turn, or payload queue overflow:

```json
{"request_id":"req-001","message":"duplicate request_id in the recent admission window"}
```

This example is the event's `data`. The rejected request ID is echoed as its
original JSON value when present, even if it was not a valid string; otherwise
it is null. Diagnostic messages are human-readable, not stable error codes.
Rejected requests are not inserted into the duplicate cache.

Overflow is reported independently of the payload consumer, including while
the current model request is suspended. Its stable code is `payload_queue_full`:

```json
{"request_id":"req-002","operation":"message","code":"payload_queue_full","message":"Worker input queue is full. Wait for current work to finish, then retry."}
```

The event's envelope `request_id` is the rejected string ID, or an empty string
when the supplied ID was not a string. Its `run_id` is empty: a rejection never
belongs to the currently executing run. The `data.request_id` remains the
authoritative correlation value. `data.operation` is retained only for recognized
`message`, `continue`, and `compact` operations. The discarded input never starts
a run, applies options, or enters conversation state. Previously queued inputs
retain their FIFO order. Wait for current work to finish before an explicit
retry; the worker and Hub never automatically resend it.

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
| `compact_finished` | `{ "summary": string, "memory_file": string, "removed_turns": unsigned integer, "revision": unsigned integer, "durable": true, "archive_cleanup"?: { "removed_archives": unsigned integer, "removed_bytes": unsigned integer }, "archive_cleanup_error"?: string }` | Compacted state was durably published; old history pages must be invalidated. Cleanup success or failure is reported separately. |
| `history_error` | `{ "request_id": any JSON value or null, "message": string }` | Invalid history query. |
| `input_admitted` | `{ "operation": string }` | Host admitted `message`, `continue`, or `compact` and assigned its run ID. The operation remains in the replayable transcript, so a continuation is not mistaken for a new user message after request bookkeeping is pruned. Older workers emitted `{}`. |
| `input_rejected` | `{ "request_id": any JSON value or null, "message": string, "operation"?: "message" \| "continue" \| "compact", "code"?: "invalid_options" \| "payload_queue_full" }` | Input failed host validation or payload queue admission; no run was started for that input. A recognized operation is retained for transcript replay even after request bookkeeping expires. |
| `run_started` | `{}` | Loop admitted the invocation. |
| `input_committed` | `{}` | New user input was integrated in memory. |
| `model_response` | Message object | One complete model response was committed in memory. It can contain tool calls and need not be the final answer. |
| `tool_calls` | Array of call objects | Calls proposed for a batch, before dispatch and security evaluation. Not proof of execution or final authorization attributes. |
| `tool_results` | Array of result objects | Complete returned batch was projected into conversation state. Results remain in call order, not completion order. |
| `persisted` | `{ "boundary": string, "format": "json" }` | A required JSON snapshot write completed successfully at the named boundary. |
| `export_error` | `{ "message": string }` | Optional Markdown export failed after successful JSON persistence. |
| `error` | `{ "message": string }`, sometimes also `"durable": false` | Control-validation or worker/storage diagnostic. Not a universal fatal-error notification. |
| `run_finished` | `{ "status": string, "error": string, "exchanges": unsigned integer, "durable": boolean, "failure"?: object }` | Invocation settled and its configured final persistence was handled. |

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
  "capabilities": ["session-history", "context-compact"],
  "memory_retention": {"max_archives": 5},
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
| `memory_retention` | Object | Effective `max_archives` (default 5); zero disables cleanup. Older workers omit this field. |
| `capabilities` | Array of strings | Features supported by this worker process. `session-history` means it accepts read-only `history` payloads; `context-compact` means it implements the `compact` lifecycle (subject to persistence and state prerequisites). A hub should check this before querying a worker that may be older than the hub. |
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

For `failed`, `failure` contains `stage` and `can_continue`. The stage is
`model_request` when the model conversation request raised before a response
was committed; other failures use `other`. `can_continue: true` means a turn
exists and the settled loop phase is `ready`, so a `continue` request may be
admitted without adding a user message. It does not promise that the next
request will succeed or that the provider is healthy. `false` calls for
inspection before retrying. Older workers may omit `failure`; clients should
treat that as unclassified and avoid automatic retry advice. `error` remains
the technical diagnostic, which clients may show on demand.

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
| `compact` | Mandatory successful compact replacement, before the success event. |
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
| `modality` | `text`, `image`, `audio`, `video`, `document` | Media category of the payload, independent of `type`. |
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
  "content": [{"type":"text","raw":"Hello.","modality":"text"}],
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
| Payload queue full | Incoming payload is discarded and `rejected_payloads` increases. Metadata-only overflow feedback emits a request-correlated `input_rejected` with code `payload_queue_full`, when the control and event paths remain usable. |
| Signal queue full | Fatal IO error; worker shutdown is initiated. |
| Overflow notification path full | Fatal IO/worker error and cleanup; the aggregate rejection count remains incremented. No unbounded notification backlog or automatic replay is created. |
| Worker event queue full | Fatal worker error, cancellation and cleanup; results are not silently discarded as if delivery succeeded. |
| Transport write queue full | Sender waits; pressure can eventually fill the worker event queue. |
| Invalid JSON, malformed envelope, unknown envelope `type`, or binary input | Fatal event-client error; no automatic reconnect for that application/protocol failure. |
| Valid envelope with invalid payload | `input_rejected`, when dequeued and the event path is usable. |
| Valid envelope with invalid signal | `error`, when processed and the event path is usable. |

Queue capacities count messages, not bytes. There is no public configurable
application-message byte limit; transport library limits and available memory
still constrain messages. Hubs should avoid sending a burst without observing
admission and should keep reading events while waiting for user decisions.

Overflow feedback shares the bounded IO control queue (`client.signal_capacity`)
with signals and history queries. A second metadata-only application queue, also
bounded by `client.signal_capacity`, hands notifications to the application strand.
The notification handler never accesses conversation state from the IO worker
thread or posts one task per rejection. A strand-owned coroutine emits ordinary
sequenced events through `worker.event_capacity`; exhausting that event queue
retains the existing fatal policy. Shutdown or an unusable response connection
may prevent feedback delivery. These notifications add no ACK, durable outbox,
delivery guarantee, or exactly-once promise.

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
`<persistence.directory>/<persistence.state>/state.json`; optional readable Markdown is
`readable.md` in the same directory. There is no protocol operation to download,
replace, edit, or reset a snapshot. A hub needing those capabilities must provide
a separate managed integration, not send invented signal operations.

`persistence.directory` is the direct session root; no session ID is appended.
`persistence.state` and `persistence.memory` default to `state` and `memory`.
Both must be nonempty relative paths without parent traversal, resolved against
that root. A relative root resolves against the startup configuration file.
The old directory layout is not automatically migrated.

A persistent worker takes an exclusive local `session.lock` in the root before restoration.
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

## Remote tool requests

This transport is independent of events and confirmation. The hub implements
the listener and rejection protocol. When configured, the worker constructs an
`HubRemoteCallToolSet` with the `plan` tool and an abstract request base.
Omission leaves the set unloaded. The set registers the plan tool and skill; construction opens no connection.

```yaml
hub_remote_call:
  endpoint: ws://127.0.0.1:8801/agent/session-1/tools?token=SESSION_TOKEN
  timeout_ms: 120000
```

The mapping is optional. If present, `endpoint` is a complete `ws://` or `wss://`
URL; `timeout_ms` is an integer in `1..2147483647` and defaults to `120000`. The bundled hub uses
plain WebSocket; a deployment may terminate TLS in front of it. The endpoint is
a base URL: append `/<route>` to its **pathname**, preserving the query.
For example, route `files/read` connects to
`/agent/session-1/tools/files/read?token=SESSION_TOKEN`. The route consists of
slash-separated lowercase segments matching `[a-z][a-z0-9_-]*`. It is a literal
identifier, not an encoded path, command, or arbitrary URL. There are no aliases
and no body field that overrides the selected route. The plan routes are defined below.

The hub authenticates the upgrade using the session's token, as for confirmation.
Unknown sessions/paths receive HTTP `404`, invalid tokens `401`, and exhausted
connection capacity or shutdown `503`. Only this dedicated listener exposes the
route. An HTTP request without an upgrade receives `404`.

One connection carries exactly one UTF-8 text JSON request:

```json
{
  "type": "tool_request",
  "data": {
    "worker_id": "worker-1",
    "session_id": "session-1",
    "run_id": "run-1",
    "request_id": "request-1",
    "arguments": {}
  }
}
```

All four identifiers must be nonempty, non-whitespace strings. `session_id` must
match the authenticated route; `arguments` must be an object. Additional fields
are tolerated but ignored. The caller chooses a distinct `request_id` per
attempt. Identifiers correlate exchanges; they provide neither authorization nor
deduplication. Executable routes require a live event connection whose worker
and active run match the request. The bundled hub waits up to
`limits.confirmIdentityHoldMs` (bounded by the connection deadline) for event
admission/status to arrive over the separate channel. It rechecks the session
and token before executing. A closed/timed-out request cannot commit after waiting.

Unknown routes receive the following terminal rejection:

```json
{
  "type": "tool_response",
  "data": {
    "worker_id": "worker-1",
    "session_id": "session-1",
    "run_id": "run-1",
    "request_id": "request-1",
    "route": "files/read",
    "status": "rejected",
    "error": {
      "code": "not_implemented",
      "message": "remote tool route is not implemented"
    }
  }
}
```

The hub echoes identifiers and the route, sends no result, then closes with
`1000`. The response is a terminal rejection, not a tool result or confirmation
decision. Unknown routes perform no side effects or emit panel events.

Binary requests close with `1003`; malformed JSON/envelopes, a mismatched session,
or additional application frames close with `1008`. Oversized frames close with
`1009`, and invalid UTF-8 with `1007`. A second frame never invokes dispatch
again; a response already sent for the first frame cannot be revoked.
`limits.maxMessageBytes` bounds incoming messages. `toolRequests.maxConnections`
bounds upgraded sockets globally (default `128`). `toolRequests.timeoutMs`
bounds the entire server-side exchange from upgrade through close (default
`120000`); expiry terminates the socket, including silent clients and peers that
never finish a closing handshake. Hub shutdown terminates these sockets without
waiting for peers. Timeout/disconnection is a transport failure, not a synthesized
application response. The worker request base enforces its configured deadline and validates echoed
identifiers. It never retries automatically.

There is no automatic retry, replay, or exactly-once execution guarantee. Before
adding executable routes, implementations must explicitly define authorization
against the live worker, per-route arguments/results, cancellation and side-effect
semantics, and whether retries/deduplication are safe. Receiving an RPC must never
implicitly approve a tool or mutate the active AgentInputState. These rules are
the extension boundary for adding operations beyond the plan routes.


### Subagent routes

The bundled Hub additionally implements `subagent/clean-fork`, `subagent/send`
and `subagent/receive` over the same one-shot remote transport. They require the
caller's live identity/active run and direct-parent ownership. Clean-fork starts
an independent headless worker from the caller's startup configuration; send
supports message/continue/compact/stop; receive returns bounded status and primary
conversation, without tools or reasoning. Mutations use bounded durable receipts
and never automatically retransmit unknown payloads. A future worker tool may
adopt these routes; no C++ subagent tool is included yet.

The [Hub subagent contract](../hub/subagents.md) defines the complete argument,
result, authorization, duplicate, configuration, approval and lifetime semantics.
Headless workers otherwise use the existing worker protocol unchanged.

### Plan routes

The `plan` tool selects `plan/read` for `{"operation":"read"}` and `plan/replace`
for `{"operation":"replace","markdown":"- [ ] Work"}`. Read forbids markdown;
replace requires it. Unknown arguments are rejected. Markdown is limited to
64 KiB UTF-8; empty or whitespace-only text clears the plan. Both operations
access only the authenticated session. The panel is read-only.

A successful response uses the same correlation fields and `route`, with
`status: "succeeded"` and an object `result` (no `error`). Read returns:

```json
{"markdown":"- [ ] Work","revision":1,"updated_at":"2026-09-29T00:00:00Z"}
```

Replace returns only `{revision, updated_at, changed}`. The initial empty plan
has revision `0` and null updated_at; unchanged content does not increment the
revision. Errors use `status: "rejected"` and `error: {code,message}` with no
result. Implemented codes are `unauthorized`, `invalid_arguments`, `storage_error`
and the unknown-route `not_implemented`.

The hub serializes plan IO and atomically publishes `plan.json` in the session
root before replying or broadcasting a panel `plan` message. Failed publication
preserves the previous file. A committed update is not rolled back by later run
cancellation or a lost reply. Read to resolve an uncertain outcome before replacing.
Plans persist independently of AgentInputState across run completion, cancellation,
compaction and restart. Panel subscription includes a full `plan` snapshot, so
recovery does not depend on retained transcript events.


## Bundled Hub implementation

The following describes the bundled Node.js Hub, not additional requirements
for every protocol implementation. Source and test paths are relative to `hub/`.
See [Hub deployment](../deployment/hub.md) for operator setup and the
[panel protocol](../hub/hub-protocol.md) for the separate browser-facing API.

### Routes and binding

| Route | Role | Lifetime | Implemented by |
| --- | --- | --- | --- |
| `GET /agent/<session_id>/events?token=<t>` | events and inputs | persistent, reconnecting | `src/worker/connection.ts` |
| `GET /agent/<session_id>/confirm?token=<t>` | one-shot confirmation | one request, one response, then close | `src/worker/confirmation.ts` |
| `GET /agent/<session_id>/tools/<route>?token=<t>` | remote tool requests | one request and response on the dedicated tool listener | `src/worker/tools.ts`, `src/worker/plan.ts` |
| anything else | — | rejected during the upgrade | `src/http/server.ts` |

The main listener (default port 8800) serves events, confirmation, and the
browser HTTP/API and `/panel/ws` surfaces. Remote tools use a separate listener
(default port 8801). Both listeners use plain `ws://`; TLS may be terminated by
a reverse proxy. The worker never speaks the browser protocol.

**Per-session paths plus a per-session token** are how the hub associates a
connection with a deployment-authorized worker and session without touching the
worker: `client.endpoint` already accepts a path and query string, and the
protocol exposes no authentication headers, cookies, or subprotocols. The token
is generated when the session is created, written to `hub.json`, and reused
across hub restarts — otherwise a running worker would be locked out by its own
hub.

What the token does and does not buy:

- It stops a connection from attaching to a session it was not configured for,
  and it stops unrelated local processes from driving the hub.
- It is a bearer credential that exists on disk in the generated worker
  configuration and in `hub.json`. It is not an authentication handshake, and
  the protocol does not define one.
- The default listener is `127.0.0.1`. A non-loopback listener is refused by
  configuration validation unless `panel.token` is set, because a payload
  channel is also an approval authority (protocol section "Connections and
  configuration"). For untrusted networks, terminate TLS at a reverse proxy and
  keep the hub on a private interface.

### Identity

The hub keeps a three-valued identity per session, not a cached `worker_id`:

| State | Meaning | Used for |
| --- | --- | --- |
| `unknown` | no event connection has delivered an event since this hub started | nothing may be decided |
| `live` | an event connection is open and has named its worker | judging confirmations |
| `stale` | a worker was identified, its connection is currently closed | nothing; `lastWorkerId` is kept only to explain what happened |

The distinction matters because a restarted worker has a new `worker_id`.
Comparing a confirmation against the last known one would deny every legitimate
prompt after a restart, so a stale identity is treated exactly like an unknown
one.

### Confirmation judgement

`confirmation_request.data.worker_id` is self-declared, and the protocol
requires a mismatch to be denied. The hub therefore judges it against the event
connection (`src/worker/confirmation.ts`):

| Identity at arrival | Result |
| --- | --- |
| `live` and equal to the request | the operator is asked |
| `live` and different | denied immediately: a stale incarnation, or a process that does not own this session |
| `unknown` or `stale` | **held** for `limits.confirmIdentityHoldMs` (default 15000 ms, clamped below the confirmation deadline) while the event connection identifies itself |

While held, the prompt appears in the panel as `awaiting-identity` with the
decision buttons disabled. If the event connection names the claimed worker, the
prompt becomes decidable; if it names a different one, the call is denied; if
nothing identifies the worker in time, the call is denied and the confirmation
connection is closed so the worker does not wait for its own deadline.

The hold exists because the two connections are independent: the protocol warns
that a confirmation can arrive before the event connection has delivered
anything. Denying immediately would fail closed but would also reject legitimate
prompts during an ordinary reconnect.

Other confirmation rules:

- Exactly one request and one response per connection. A second application
  frame aborts the exchange (close 1008) without answering; a duplicate
  `confirmation_id` is refused without disturbing the first exchange.
- All four identifiers are echoed exactly as received, with `decision` and an
  optional `reason`. The worker validates them and denies on any mismatch, so
  the hub never rewrites them.
- The decision is written, then the panel is notified, then the hub waits for
  the worker's close handshake (3 s, then the socket is terminated). The
  transport finishing must not delay what the operator sees.
- `deadline_at` in the panel is advisory: the worker's deadline started before
  the connection existed. The prompt is retired on deadline, on disconnect, and
  on hub shutdown; a retired prompt cannot be answered later.

### Remote tools

The remote-tool listener authenticates the session token before accepting an
upgrade. Plan and subagent routes additionally require a live event identity
and matching active run. They wait briefly for independently delivered event
admission/status, then reject if that identity cannot be established. Unknown
routes receive `not_implemented` and perform no operation.

Plans are stored in the session root as `plan.json`. Replacement is published
before a successful reply and panel broadcast; cancellation or a lost reply does
not undo it. A fresh panel subscription includes the full plan independently of
the event transcript. See the [remote-tool contract](#remote-tool-requests)
for limits, correlation, and failure semantics.

### Events

- `status` is requested immediately after every accepted upgrade. `ready` is
  emitted once per worker process, so a reconnect must not wait for it.
- `(worker_id, sequence)` is tracked per connection: gaps and duplicates are
  counted per session and shown in the panel. Sequence numbers are read
  losslessly, so a 64-bit value that `JSON.parse` would round still orders
  correctly.
- Unknown event names and unknown fields are preserved and rendered generically.
  The hub never disconnects because a future worker emits something new.
- A binary message, an unparseable document, a missing identity, or an
  `session_id` that disagrees with the route increments the session's protocol
  error counter. A fatal error closes the connection (1008) once the connection has recorded
  at least twenty issues, including non-fatal ones. Non-fatal
  issues (missing `request_id`, `run_id`, `sequence`, or `data`) are recorded on
  the envelope and the event is still surfaced.
- A WebSocket ping runs every `limits.pingIntervalMs` (default 30 s) and a
  missed pong terminates the socket, so a half-open connection stops looking
  like a live worker.

#### A second event connection supersedes the first

The deprecated `simplex_shell` example answered a second event connection with HTTP 409. This hub does
not: it closes the previous connection (code 4001) and accepts the new one.

The reason is operational. A worker that reconnects after an unobserved peer
death is the common case, the worker retries a rejected upgrade forever anyway
(with capped backoff and no attempt limit), and two live workers on one session
are already prevented by the worker's own session lock. Rejecting the upgrade
would leave the hub unable to talk to the worker that is actually running. The
replacement is logged at warn level and the panel shows the new connection.

### Inputs and signals

- `request_id` is generated by the hub when the panel does not supply one, and
  the payload is validated locally first (`src/protocol/messages.ts`, mirroring
  the worker's own rules) so a mistake is reported before a round trip. The
  worker remains authoritative: `input_rejected` is surfaced exactly as sent.
- A send is only `sent` until `input_admitted` or `input_rejected` is observed.
  If the connection drops in between, the outcome becomes `unknown` and the
  panel says so. It is never retried automatically: the protocol has no delivery
  acknowledgement, and a failed write has an unknown outcome.
- `cancel` uses the `run_id` the panel names, defaulting to the most recent one
  observed on the event stream. A stale id is harmless (the worker ignores it),
  which is why the default is safe rather than clever.
- `shutdown` is used as the first step of every graceful stop.

### Process lifecycle

`src/launch/supervisor.ts` owns worker processes. The order is always
**protocol first**:

1. `{"type":"signal","data":{"operation":"shutdown"}}` and wait
   `worker.stopTimeoutMs` (default 15 s),
2. `SIGTERM` and wait `worker.sigtermGraceMs` (default 5 s),
3. `SIGKILL`, and with `forceKillProcessGroup` (off by default) or an explicit
   force-kill from the panel, the signal goes to the whole process group.

Protocol-first is what makes an orphan controllable: a hub that restarted
without its process table can still stop a worker it can reach over the socket.

Adoption after a restart uses the recorded pid **and** the `/proc/<pid>/stat`
start time, so a pid that has been reused is never signalled. Adoption fails
closed: without a recorded start time the process is not adopted at all.

`SIGKILL` to the process group reaches descendants the worker itself does not
promise to terminate. It is never used implicitly.

### Persistence

The hub stores sessions, launch specs, tokens, and process identity in
`hub.json`. Conversation history is deliberately absent — that is the worker's
own snapshot. The panel queries a simplified history projection from a connected
worker advertising `session-history`; those replies are forwarded live without
being retained in the event transcript. The Hub also provides a local snapshot
inspection endpoint, which can *read* `<persistence.directory>/<persistence.state>/state.json` and
`readable.md`; there is no operation to replace, edit, or reset a snapshot,
because the protocol defines none and inventing one would need a managed
integration on the worker side.

The hub's own event transcript (`<dataDir>/sessions/<session>/events.jsonl`) is an
operator artifact. A hub restart starts an empty in-memory transcript instead of
replaying the file, so panel replay is scoped to one hub process.

### Requirement-by-requirement

The protocol's "Hub implementation requirements" list, and where each one
lives. "Deployment" means the hub provides the mechanism but the operator owns
the policy.

| # | Requirement | Implementation | Notes |
| --- | --- | --- | --- |
| 1 | Accept worker text WebSockets, implement ping/pong and close | `src/http/server.ts`, `src/worker/connection.ts` | `ws` handles control frames; the hub adds an application ping interval. Routes are not reserved paths. |
| 2 | Associate connections with an authorized worker/session; treat payload senders as approval authorities | `src/http/auth.ts`, `src/state/registry.ts` | Per-session token in the query. Deployment owns the trust boundary around `/panel/ws` and `/api/*`; a non-loopback listener requires a panel token. |
| 3 | Generate valid request IDs, wait for outcomes, never retry side-effecting work | `src/state/registry.ts`, `src/panel/api.ts` | `sent` / `admitted` / `rejected` / `unknown` are all visible; there is no automatic resend. |
| 4 | Decode every event shape, treat text as untrusted, tolerate unknown fields | `src/protocol/events.ts`, `web/src/app/Transcript.tsx` | Unknown events render generically; the panel never writes markup as HTML. |
| 5 | Service confirmations concurrently, validate correlation, close promptly, retire disconnected prompts | `src/worker/confirmation.ts` | Several prompts per session are supported; identity is judged as described above. |
| 6 | Target cancellation by run ID and wait for settlement | `src/panel/api.ts` | The panel offers cancel only with a known run id, and shows `run_finished` as the settlement. |
| 7 | Keep reads flowing, track sequence gaps and overflow counters, send `status` after reconnect | `src/worker/connection.ts` | Gaps, duplicates, `rejected_payloads`, and `storage_failed` are surfaced separately. |
| 8 | Surface run failure, storage failure, recovery phases, and connection loss distinctly | `web/src/app/*`, `src/state/registry.ts` | `tools`/`blocked` phases, `storage_failed` and the socket state are called out; an unknown request outcome is never shown as success. |

### Known limits

- No delivery acknowledgement exists, so the hub cannot prove that a payload was
  admitted, executed, or persisted from a successful socket write alone.
  Admission, run, and persistence events report those outcomes when received;
  missing events leave the corresponding outcome unknown.
- The hub's in-memory transcript does not survive a restart; the JSONL log does,
  but it is not replayed into the panel.
- A confirmation's local deadline is advisory, as explained above.
- Cancellation does not roll back accepted tool calls; the hub says so in the UI
  rather than implying otherwise.
- The Hub has no native TLS listener. Workers support `wss://`; terminate TLS
  at a reverse proxy when using it.
- The panel has no user accounts or roles. One optional shared token guards the
  browser surface; any authenticated panel client (or any client when no token
  is configured) has approval and launch-configuration authority.
- Orphan detection for a worker started outside the hub is limited to what the
  protocol allows: the hub can drive and stop it, but it has no process record
  and the panel marks it as unattached.
