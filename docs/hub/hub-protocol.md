# Hub panel protocol, version 1

The hub's own client protocol: what a browser (or any operator tool) exchanges
with the hub to manage sessions, read worker events, answer confirmations, and
control worker processes.

This is **not** the worker protocol. The worker-facing contract stays in
[`docs/core/worker-protocol.md`](../core/worker-protocol.md); a browser
never speaks it, because the worker-facing payload channel is an approval
authority and must stay a deployment-trusted endpoint. How the hub implements
the worker side is described in [worker-adapter.md](worker-adapter.md).

Source and test paths on this page are relative to the `hub/` package.

## Transport and versioning

| Surface | Path | Notes |
| --- | --- | --- |
| The panel | `GET /` | the built browser client, served from `web/dist` |
| Metadata | `GET /api/meta` | protocol version, capabilities, provider profiles |
| JSON API | `/api/...` | management and read-only queries |
| Panel socket | `GET /panel/ws` (WebSocket) | live events, prompts, and commands |

The panel is one page with no client-side routing, so every path under `/` that
is not `/api` or `/panel/ws` is a static file request and nothing needs a
fallback. Anything else — a wrong path, a missing asset — is a `404`, and a hub
whose panel has not been built answers `503` with the command that builds it.

Every panel WebSocket message carries `"v": 1`. The hub ignores unknown message
types and preserves unknown fields, so a newer panel may talk to an older hub and
the reverse. A message with an unrecognised `v` is answered with
`unsupported_version` instead of being misinterpreted.

Additive change is the rule: new message types, new optional fields, and new
values inside existing enums. Removing or reinterpreting a field needs a version
bump. `GET /api/meta` reports `protocol.version` and a `capabilities` list so a
client can check for a feature instead of guessing.

### Outbound backpressure and recovery

Each panel connection has a 4 MiB outbound budget. Before sending, the hub
checks the connection's pending bytes plus the next message's complete UTF-8
JSON (including `v` and escaped values) and unmasked WebSocket frame header.
Compression is disabled. A single frame larger than this budget is also
rejected, even if the connection has no pending output.

If a send would exceed the budget, the hub immediately terminates that panel
connection and logs the byte counts. It does not wait for a close handshake or
silently discard individual messages while leaving the connection open. The
client normally observes abnormal closure (`1006`); no error frame or close
reason is guaranteed because the peer may have stopped reading. Other panel
connections, worker event ingestion, and tool confirmations continue normally.

The bundled panel reconnects with backoff and subscribes using its transcript
cursor and epoch. When the hub advertises `transcript-pages`, initial load,
reconnect, and explicit refresh use the paced replay described below, so an
aggregate transcript larger than 4 MiB can recover without a reconnect loop.
Supported worker history is queried after replay completes; open confirmations
are refreshed through session snapshots. These mechanisms remain bounded by
the retained transcript and worker history contract; they are not delivery
acknowledgements. Never automatically resend an input or confirmation decision
because of a disconnect: it may already have been processed.

An individual envelope or subscription metadata object larger than the hard
budget still cannot be sent. Neither can an oversized legacy `subscribed` or
`snapshot` response requested without paged replay. Reconnecting alone cannot
make these frames fit. Operator clients should opt into `transcript-pages`,
choose a narrower `since`, or inspect event/log tails through the JSON API
(`limit`). The bundled panel falls back to legacy requests for older hubs that
do not advertise the capability; those builds do not provide paged recovery.

### Capabilities

| Capability | Meaning |
| --- | --- |
| `worker-events` | worker envelopes are forwarded, unknown event names included |
| `confirmations` | tool confirmations are surfaced and can be answered |
| `supervisor` | worker processes can be started, stopped, and force-killed |
| `transcript-replay` | `subscribed` replays the transcript after a cursor |
| `transcript-pages` | opt-in subscription replay uses paced, byte-bounded pages |
| `snapshot-view` | the worker's persisted snapshot can be read, never written |
| `transcript-epoch` | `transcript_epoch` is reported, so a stale cursor is detectable |
| `global-confirmations` | confirmations reach every client, not only subscribers |
| `answer-pages` | read exact worker answer content through authenticated bounded pages |
| `session-history` | panel can query a worker's simplified conversation history |
| `context-compact` | panel can request worker context compaction and display the result |
| `headless-subagents` | authenticated parent delegation and headless operator safety controls |

These describe the hub **build**, not its configuration. `supervisor` means
"this hub starts and signals worker processes", which stays true for every
launcher. A capability that varied with
configuration would be a different kind of list, and nothing in this one does.
Metadata retains `launcher.owns_config` as a deprecated protocol-v1 field. It
is always `false` because the hub now owns the saved worker configuration for
every launcher; v1 clients can continue to read the original shape.

`shared/protocol.ts` is the machine-readable copy, and
`test/panel-protocol-drift.test.js` fails when this table and that module
disagree.

### Authentication

Panel authentication is a single optional shared token (`panel.token`):

```
?token=<t>                      query parameter (also used for the WebSocket)
Authorization: Bearer <t>       header
Cookie: simplex_hub_token=<t>   cookie
```

When no token is configured, the panel is open, and configuration validation
only permits that for a loopback listener. A 401 from the API, or a rejected
WebSocket upgrade, means the token is missing or wrong.

`GET /api/meta` is deliberately readable without a token: the panel needs it to
render a token prompt, and it exposes only the protocol version, capabilities,
listener address, launcher kind, provider profile *names*, and whether the mock
provider and process-group kill are enabled — no paths and no credentials.

A WebSocket upgrade whose `Origin` header is present and does not match the
request's `Host` is refused with 403, which is what stops a visited web page
from driving a loopback hub.

Worker routes use different, per-session tokens; a panel token never works
there.

## Session description

Descriptions include `kind` (`ordinary` or `headless`). A headless description
also includes `subagent` with parent, lifecycle, policy, health/reason,
`observed_at` and active-run flag. Its spec/invocation are redacted; no raw event
cache or full conversation is exposed. See [headless subagents](subagents.md).
Headless sessions permit description/listing, `confirmation` and
`subagent_policy`; subscription, replay, logs/snapshot, direct input/history,
signals, worker control and configuration replacement are rejected on both APIs.


Most messages embed this object, produced by `Session.describe()`:

```json
{
  "session_id": "demo",
  "created_at": "2026-01-01T00:00:00.000Z",
  "spec": {
    "provider": "mock", "model": "", "threads": 1, "maxExchanges": 512,
    "systemPromptFile": "prompts/coding_agent.yaml",
    "workspace": "", "platform": "", "software": [],
    "persistence": {"enabled": true, "readable": false}, "restore": "if_present",
    "env": {}, "extraArgs": []
  },
  "connected": true,
  "worker_capabilities": ["session-history"],
  "identity": {"state": "live", "worker_id": "204d23ea-...", "since": "..."},
  "stats": {"events": 12, "gaps": 0, "duplicates": 0, "protocolErrors": 0, "incarnations": 0},
  "last_run_id": "fd473c9f-...",
  "last_event_at": "2026-01-01T00:00:01.000Z",
  "last_event": "tool_results",
  "confirmations": [],
  "process": null,
  "requests": []
}
```

`process` is present when a worker process is known:

```json
{
  "state": "running", "pid": 4242, "started_at": "...", "exited_at": null,
  "exit_code": null, "signal": null, "error": null, "stop_requested": false,
  "command": "/path/simplex_worker", "args": ["--config", "..."], "cwd": "...",
  "process_group_killed": false, "log_path": "...", "log_lines": 12, "log_dropped": 0,
  "log_truncated_bytes": 0, "file_log_dropped": 0, "file_log_failed": false
}
```

The three additional log diagnostics are optional for compatibility with older
hubs. `log_dropped` counts lines evicted from the in-memory ring;
`log_truncated_bytes` counts decoded UTF-8 bytes removed from oversized lines;
`file_log_dropped` counts records rejected by disk admission, independently of
the memory ring. `file_log_failed` means disk logging has been disabled for this
process. An admitted write is not proof of delivery or durability.

`requests` records payloads the hub sent, keyed by `request_id`, with
`state` one of:

| State | Meaning |
| --- | --- |
| `sent` | written to the socket; nothing observed yet |
| `admitted` | `input_admitted` observed for this id |
| `rejected` | `input_rejected` observed; `detail` carries the worker diagnostic |
| `unknown` | the connection dropped before either was observed |

`unknown` is not a failure and not a success. The hub never resends, and a
client must not present it as either.

Worker payload overflow reports `input_rejected.data.code = "payload_queue_full"`
with the discarded request's ID. The Hub settles that request as `rejected`,
retains its diagnostic for replay, and leaves other admitted requests alone.
The panel labels its pending message rejected and displays the worker's advice
to wait for current work to finish before retrying. The rejection carries an
empty `run_id`; it does not finish or rename the active run. If the response
path fails before the rejection is observed, the outcome remains `unknown`.
Neither the Hub nor panel automatically resends inputs.

The panel keeps queued inputs separate from executing runs. Sending an input or
receiving a Hub request record does not open a run. Worker admission and
execution events bind the input by `request_id`; subsequent results follow
`run_id`, scoped to `worker_id`. A queued or rejected input cannot take over
another run's model replies, tool results, or completion. The same association
applies to live events and transcript replay.
Executed rounds follow worker admission order, even if another panel's earlier
input is observed after a local outbox was created. History matching, latest-run
selection, and default folding use that same execution order.

`spec` contains the session's resolved launch/model settings. Sessions created
from the configuration library additionally identify their saved sources with
`launchConfig` and `workerConfig`; these select session snapshots rather than
live links to the library. On worker startup, the model and optional
`modalityAssistProvider` fields are refreshed from the persisted worker
configuration. `spec` is not a complete copy of that YAML document.

A confirmation prompt object:

```json
{
  "confirmation_id": "573e354f-...", "session_id": "demo", "worker_id": "...",
  "run_id": "...", "state": "awaiting-decision", "verified": true,
  "identity_state": "live",
  "call": {"type": "serial_write", "security": "require_confirm", "id": "call-001",
           "name": "run_command", "arguments": {"command": "..."}},
  "received_at": "...", "deadline_at": "...", "settled_at": null,
  "decision": null, "reason": null
}
```

`state` is `awaiting-identity`, `awaiting-decision`, `decided`, or `retired`.
`deadline_at` is advisory: the worker's own deadline started before the
confirmation connection existed.

Argument display copies preserve up to 64 KiB of encoded JSON per individual
confirmation event or REST description. Aggregate WebSocket descriptions
(`welcome`, `sessions`, `session`, `created`, `subscribed`, `snapshot`) share
512 KiB across all included approvals and sessions, with a smaller allowance
when other contents approach the 2 MiB frame ceiling. Allocation preserves small
previews and reduces larger ones first. Prompt identities and lifecycle fields
are retained; previews are regenerated from the original pending calls.
Shortened prompts set `arguments_truncated: true` and `arguments_bytes` to the
original argument JSON size. Strings use
`{"display_truncated":true,"bytes":N,"preview":"..."}` and omitted values use
`{"display_omitted":true}` (array tails can also carry `omitted_items`).
Snapshot previews can therefore be shorter than the same prompt's individual
event or REST description. A decision always applies to the original operation.

## JSON API

| Method and path | Body | Response |
| --- | --- | --- |
| `GET /api/meta` | — | hub metadata and capabilities |
| `POST /api/sessions/:id/subagent-policy` | `policy` | authenticated operator changes headless ask/deny/approve policy |
| `GET /api/sessions` | — | `{sessions: [session...]}` |
| `POST /api/sessions` | `{session, spec?}` | `201 {session}`; `400 invalid_session`; `409 session_exists` |
| `GET /api/sessions/:id` | — | `{session}`; `404 unknown_session` |
| `DELETE /api/sessions/:id` | — | `{removed}`; removes the complete session directory, including config, state, memory, logs and tool-created files within it; `409 session_busy` while a worker runs or is connected |
| `POST /api/sessions/:id/start` | `{spec?}` | `{ok, pid?, config}`; `409` with `{ok:false, error}` |
| `POST /api/sessions/:id/stop` | — | `{ok, how, forced}` — `how` is `shutdown-signal`, `sigterm`, `sigkill`, `sigkill-process-group`, `already-exited`, or `not-started` |
| `POST /api/sessions/:id/restart` | `{spec?}` | start result plus `stop` |
| `POST /api/sessions/:id/force-kill` | — | `{ok, how, forced}`; skips the protocol and signals the process group |
| `GET /api/sessions/:id/events?since=&limit=` | — | `{session, since, latest, events}` |
| `GET /api/sessions/:id/logs?limit=` | — | `{session, lines, dropped, log_path}` |
| `GET /api/sessions/:id/snapshot` | — | `{session_id, state, readable, files}` |

Errors are `{"error": "<code>", "message": "<human readable>"}` with a 4xx
status. `error` codes are stable; `message` is not.

`events` returns the hub's retained transcript in `hub_sequence` order.
`hub_sequence` counts retained transcript envelopes in *this hub process*,
which is what a client resumes from. `latest` is the current end of the transcript.

New sessions may select saved files with
`spec: {launchConfig: "local", workerConfig: "default"}`. Both selectors are
required together. The Hub captures independent launch/worker snapshots; each
session uses its own launcher. Configuration CRUD, endpoint preview and explicit
snapshot replacement are documented in [Configuration API](configurations.md#configuration-api).
Those routes have the same panel authentication requirements as this API.

Start and restart reuse `sessions/<session>/config/config.yaml` once it exists.
Only the hub-owned session root, connection URLs/tokens and active mock URL are
refreshed. Later spec fields affect only launch parameters (`threads`, `env`,
`extraArgs`) for legacy sessions. Library-backed sessions instead use their
saved launch snapshot for threads and environment; apply new snapshots while
stopped to change their configuration.
`persistence.state` and `persistence.memory` select relative subdirectories of
`persistence.directory`, which the hub sets to `<dataDir>/sessions/<session>`.
Old layouts are not migrated automatically.

`snapshot` reads the worker's own files
(`<persistence.directory>/<persistence.state>/state.json` and `readable.md`) without
modifying them. The worker owns that state; there is no operation to replace,
edit, or reset it. Files larger than 8 MiB are skipped rather than streamed.

### Worker output and optional disk logs

Worker stdout and stderr have independent UTF-8 decoders and partial-line
buffers. Completed lines share the captured log tail in arrival order; fragments
from different pipes are never joined. Each line retains at most a 64 KiB
decoded UTF-8 prefix without splitting a code point. Extra bytes are discarded
until LF or EOF, and the retained line ends with
`[hub: truncated N UTF-8 bytes]`. The decoder is flushed at pipe completion,
including an incomplete final character as `�`. CRLF is normalized and its
terminator is excluded from the content budget and truncation count, including
when CR and LF arrive in separate chunks. After child
exit, the Hub allows up to one second for pipe EOF, then closes inherited pipes
and flushes the retained fragments. Process state changes at exit independently
of this best-effort output drain.

The existing `limits.logLines` and `limits.logRingBytes` bound the captured tail;
the byte measure uses UTF-8. As with other rings, a single entry may exceed a
smaller ring budget. The per-line prefix cap still applies.

`logs/worker.log` and `events.jsonl` are optional operator artifacts. Each file
has at most 1 MiB of admitted UTF-8 data pending in its Writable, with no second
application queue. A write that returns `false` has been admitted; further
records are dropped until `drain`. A record that would exceed the byte budget
is dropped whole, even when it is the only record. The Hub does not pause worker
pipes or live WebSocket event routing to wait for disk.

When the sink can accept data again, omissions are reported before later
records: worker logs use `[hub: omitted N worker log records (B UTF-8 bytes)]`;
transcript files use a separate JSONL record:

```json
{"type":"hub_log_omission","dropped_records":12,"dropped_bytes":3456}
```

This diagnostic is not a worker event and has no `hub_sequence`. File omissions
do not remove envelopes from in-memory replay or change its sequence numbers.
The first omission emits a Hub warning. An open/write failure emits a warning
and disables that file writer for the rest of its lifetime; it does not
repeatedly retry a broken path. The shutdown flush has a one-second deadline,
after which a stalled file sink is destroyed. Accepted writes may still be
lost on failure or shutdown, and an omission marker itself is best effort.
These files are neither audit logs nor authoritative conversation storage;
the worker's persisted state remains the source of truth.

## Panel WebSocket

### Client to hub

| Message | Fields | Effect |
| --- | --- | --- |
| `subscribe` | `session`, optional `since`, `paged`, `replace`, `request_id` | sends `subscribed` with the transcript after `since`; `paged: true` opts into paced replay when `transcript-pages` is advertised |
| `unsubscribe` | `session` | stops live messages for that session |
| `list_sessions` | — | answers with `sessions` |
| `create_session` | `session`, optional `spec` | answers with `created`, or `session_exists` / `invalid_session` |
| `delete_session` | `session` | removes the complete session directory, then answers with `session_removed`; refused while busy |
| `worker` | `session`, `action`: `start`\|`stop`\|`restart`\|`force-kill`, optional `spec` | answers with `accepted` (carrying the result) or `worker_action_failed` |
| `input` | `session`, `operation?`: `message` \| `continue` \| `compact`, optional `content`, `request_id`, `options` | Omitted `operation` means `message`. `message` requires content; `continue` and `compact` omit it. Validates, sends a payload, answers with `accepted` and `request_id` |
| `history` | `session`, optional `request_id`, `start`, `step`, `limit` | if the current worker advertises `session-history`, sends a read-only payload; otherwise returns `input_not_sent`. The response arrives as a transient `history` worker event |
| `signal` | `session`, `operation`: `status`\|`options`\|`cancel`\|`shutdown`, optional `run_id` | answers with `accepted` or `signal_not_sent` |
| `confirmation` | `session`, `confirmation_id`, `decision`, optional `reason`, `request_id` | answers with `accepted` or `confirmation_rejected` |
| `logs` | `session`, optional `limit` | answers with up to 2000 captured worker lines |
| `status_snapshot` | `session`, optional `since` | answers with a fresh `snapshot` |
| `ping` | — | answers with `pong` |
| `subagent_policy` | `session`, `policy`: `ask`\|`deny`\|`approve` | updates only new headless confirmation requests; existing prompts remain actionable |

Approval submissions may include a unique `request_id` for each attempt. The Hub
echoes it in `accepted`; a rejection includes a bounded diagnostic `request`, including
this ID. A transport acknowledgement is separate from the authoritative
`confirmation` settlement. The panel locks only the submitted prompt, keeps
its geometry unchanged, and never automatically resends a security decision.
After eight seconds without settlement, it queries `GET /api/sessions/:id`.
A still-open prompt permits an explicit retry; absence only means the prompt
is no longer open, not that a tool succeeded. If this check fails, **Check
outcome** (in the existing primary action slot) or **Review** retries the check
before enabling a decision. The check has its own
eight-second network deadline. Panel or worker disconnects
invalidate the attempt's connection assumptions; reconnect checks the outcome
before retry. This also revokes retry permission from an earlier rejection or
still-open check. A socket becoming open alone never restores that permission;
the fresh authoritative check must finish first. Prompt creation and worker
identity fence late responses.


`input` accepts the same content parts as the worker protocol (`type` of
`text`, `binary`, or `external_ref`, a required `modality` of `text`, `image`,
`audio`, `video`, or `document`, and optional `extras`) and the same option
categories
(`model`, `tools` — reserved and empty, `confirmation.mode`). The hub validates
them locally so the panel can report a mistake immediately; the worker is still
authoritative and its rejection is surfaced unchanged. The worker requires the
`modality` field on every part and never infers it from `type`, so the hub refuses a part
without one rather than assuming text. Today's panel sends `modality: "text"`;
the attach entry stays disabled until it can ask for the category.
For `operation: "compact"`, omit content. The hub requires the current worker
to advertise `context-compact`, tracks the request like other runs, and forwards
`compact_finished` for display and replay. The panel shows the saved summary
without a user bubble, invalidates old history pages, and queries the new revision.
It does not invalidate history on failed or cancelled attempts. The current
worker's `memory_retention` status describes cleanup, and any optional
`archive_cleanup_error` in the result is displayed beside the saved summary.

`signal` with `operation: "cancel"` defaults `run_id` to the most recently
observed one. A stale id is ignored by the worker, so the default is convenient
rather than dangerous.

`history` pages are a bounded display projection of the worker's in-memory
`UserLoopStep` turns. The built-in worker limits the complete compact JSON page
data to 252 KiB and the complete worker event to 256 KiB, accounting for escaped
user text, response content, reasoning and metadata. The Hub omits `raw.data`; `raw` contains bounded envelope diagnostics only.
A full panel event stays below the 4 MiB frame/backlog ceiling. Native extras and
reasoning use bounded display previews before retention and replay. These byte limits exclude WebSocket frame headers and pretty printing.
They do not replace the general 4 MiB outbound budget for other events or impose
the new worker bound on older/custom workers.

Pages can end at a turn boundary or partway through its model steps. Follow both
`next` and `next_step`; do not assume a response contains the requested number of
turns. A continued turn repeats its user projection, which the panel keeps once
while appending its steps. Empty-step turns consume page space and advance the
turn cursor. A nonterminal page must advance, and `revision` changes still require
discarding partial results and restarting. No wire fields or capabilities change.

The hub accepts a history query only while the current
worker connection has advertised `session-history` in `ready` or `status`.
The current worker's capabilities are exposed as `worker_capabilities` in the
session description (`null` until known); they do not depend on old events
remaining in the bounded replay transcript. The browser queries on initial
subscription, worker recovery, or explicit refresh. Ordinary completed runs
are already visible through their live events and do not trigger a full reload.
The browser validates each page before committing it or following its cursor,
and restarts pagination if the revision changes between pages. The hub forwards
history replies live without retaining them in the transcript or JSONL log;
they do not consume the normal event budget or advance its replay cursor.
The authoritative restorable history remains the worker's `state.json`.
An offline worker cannot answer a live history query; the panel keeps its last
displayed page and offers a refresh after reconnection.

### Hub to client

For a hub advertising `transcript-pages`, send `subscribe` with `paged: true`
and a fresh `request_id`. It replies with one or more `subscribed` frames:

- Data pages carry `replay_more: true`. `latest` is the last `hub_sequence`
  included in that page, **not** the end of the whole retained transcript.
- A final frame carries `replay_more: false` and an empty transcript. Its
  `latest` is the fully replayed cursor; live session events follow this frame.
- Each reply echoes `request_id`. Ignore replies to an older subscription or
  to a session that has been unsubscribed. Replacing a subscription,
  unsubscribing, or closing the socket invalidates its pending server work.
- `replace: true` requests a fresh display transcript, usually with `since: 0`.
  Only its first reply carries `replay_reset: true`; replace the displayed
  transcript with that page, then merge subsequent pages. The bundled panel
  uses this instead of a single `status_snapshot` frame for explicit refresh.

Normal pages target at most 512 KiB of serialized JSON plus frame header,
including subscription metadata and escaped values. An individual envelope
larger than that target is sent alone if it fits the hard 4 MiB budget. The hub
awaits each local socket write before sending another page; this paces output
without promising remote delivery. Live session delivery is enabled only when
replay catches up. Events received during replay are included in a following
page rather than overtaking earlier history. Global session/confirmation
notifications remain available while replay is pending.

Advance a recovery cursor only for pages actually received. Wait for the final
frame before issuing worker-history queries or treating the subscription as
ready. Retention can evict older events; existing gap/epoch rules still apply.
Legacy `subscribe` requests omit `paged` and receive the original single-frame
reply, with no `replay_more` field. New clients treat that as complete; older
clients need not understand paging unless they opt into it.

| Message | Fields | Meaning |
| --- | --- | --- |
| `welcome` | `hub`, `sessions` | sent once per connection |
| `sessions` | `sessions` | full list, on request |
| `session` | `session` | one session changed |
| `session_removed` | `session` | removed from the panel session list |
| `subscribed` | `session`, `transcript`, `logs`, `latest`, `transcript_epoch`, `plan`, optional `replay_more`, `replay_reset`, `request_id` | subscription replay page or completed legacy subscription |
| `created` | `session` | session created by this client |
| `event` | `session`, `hub_seq`, `envelope` | one bounded worker display event |
| `confirmation` | `session`, `open`, `confirmation`, and `outcome` when closing | prompt opened or retired/answered; sent to **every** connected panel, not only subscribers of that session |
| `process` | `session`, `process` | worker process state changed |
| `connection` | `session`, `connected`, `identity` | event connection opened or closed |
| `request` | `session`, `request` | payload outcome changed |
| `logs` | `session`, `lines`, `dropped` | log tail, only in response to `logs` |
| `snapshot` | `session`, `transcript` | transcript plus description, on request |
| `accepted` | `action`, `session`, plus action-specific fields | the command was accepted |
| `error` | `error`, `message`, optional `request` | the command was refused |
| `plan` | `session`, `plan` | latest persisted session plan; an empty markdown string clears it |
| `pong` | `at` | heartbeat reply |

`envelope` is the worker's envelope with hub-added fields:
`hub_sequence`, `received_at`, `known`, `issues`, `raw` (bounded envelope metadata without the data body
as received). Unknown event names are forwarded exactly like known ones; the
panel decides how to render them.
`history` responses are transient control replies. Live subscribers receive
their full envelopes, but replay contains no history response. Clients issue a
fresh `history` query to recover the display projection.

`subscribed.plan` and the `plan` message carry
`{markdown: string, revision: number, updated_at: string | null}`. The initial
empty plan has revision `0` and a null timestamp. Empty or whitespace-only
Markdown clears the plan display. The plan is persisted independently of worker
conversation state and survives compaction. Re-subscribe to recover its latest
value; replaying worker events alone does not recover it.

**Open confirmations are read from the session description**, not from a field
on `subscribed`. `SessionDescription.confirmations` is the authoritative list of
what is open at the moment the description was built, and it arrives in
`welcome`, in `sessions`, in `session`, and in `subscribed`'s `session`. A client
that instead waited for a `confirmations` array on `subscribed` would show
nothing after a reload, because no such field has ever been sent.

Live messages are only sent for sessions a client has subscribed to, with one
deliberate exception: `confirmation`. An approval is the one message that must
not be missed, so it reaches every connected panel regardless of subscription.
Scoping it to subscribers made a prompt unanswerable whenever the operator
happened to be looking at another session — the only trace of it was a count in
the session list, and the worker's own deadline denied it. Every client on this
socket already shares the panel token, so widening the audience grants no
authority that was not already there.

Session list updates (`session`, `session_removed`) go to every connected
client.

A headless subagent is removed from the panel list once shutdown and persistence
cleanup succeed. `GET /api/sessions`, `sessions` and `welcome` omit these stopped
children. The Hub still retains their terminal status temporarily for the
parent's `subagent/receive` requests; `session_removed` does not imply that this
internal cache has expired. Stopping and cleanup-pending children remain listed,
as do ordinary sessions whose workers have exited.

### Replay cursors and the transcript epoch

`hub_sequence` counts retained transcript envelopes in *this hub process*, so it starts
again at 1 after the hub restarts. A client that resumes with `since=<n>`
captured before a restart would therefore receive an empty transcript, which is
indistinguishable from a session that has been idle — a silent failure that
looks like normal operation.

`transcript_epoch` is what makes the two distinguishable. `/api/meta`, `welcome`
(`hub.transcript_epoch`), and every `subscribed` reply carry it. When it differs
from the value a client last saw, the client discards its cursor and asks for the
transcript from the beginning (`since` omitted or `0`). The value is a fresh
identifier per hub process and has no other meaning; clients must treat it as
opaque.

### Error codes

| Code | Meaning |
| --- | --- |
| `bad_json` | the message was not JSON |
| `bad_message` | the message was not a JSON object |
| `unsupported_version` | `v` is not 1 |
| `unknown_session` | no such session |
| `invalid_session` | the session id is not 1–128 `[A-Za-z0-9_-]` |
| `session_exists` | the id is taken |
| `session_busy` | a worker is running or connected; stop it first |
| `headless_restricted` | direct panel conversation/control is unavailable for headless workers |
| `invalid_policy` | safety policy operation requires a headless session |
| `input_not_sent` | validation failed or the worker is not connected |
| `signal_not_sent` | same, for signals |
| `unknown_confirmation` | that prompt is no longer open |
| `confirmation_rejected` | the decision was refused (already answered, or identity unverified) |
| `worker_action_failed` | the launcher or supervisor refused the action; `result.error` explains |
| `binary_not_supported` | panel messages must be text |
| `internal_error` | the hub failed to process the message; it is still serving |

## Trust boundary

Any authenticated panel client (or any client when no token is configured) can
submit payloads, and a payload may select
`confirmation.mode: approve`, which is equivalent to approving every tool call
that requires confirmation. Panel access also allows editing launch commands
that the Hub executes on its host. Treat the panel token as an administrative
credential with host command-execution authority. The hub therefore:

- defaults to a loopback listener, and refuses a non-loopback one without a
  panel token,
- refuses cross-origin WebSocket upgrades,
- keeps the worker-facing channel separate, with its own per-session tokens,
- validates and refuses malformed or oversized requests instead of forwarding
  them.

It does **not** implement user accounts, roles, audit logs, or TLS. Deployments
that need those should terminate TLS and authenticate at a reverse proxy, and
keep the hub on a private interface.

## Session plans

`subscribed.plan` is an authoritative `{markdown, revision, updated_at}` snapshot
of the hub-owned session plan. The hub subsequently pushes `type: "plan"` with
`session` and `plan` to subscribers after a successful replacement. A revision is
monotonic within the saved session document; panels ignore older live updates,
but replace their cached value on subscription, including after hub restart.
An empty markdown string hides the plan tab. Old hubs may omit the snapshot,
which panels treat as empty. Plan updates are not transcript events.

Plans persist in `<dataDir>/sessions/<session>/plan.json`. Only the current active
worker can read/replace them via the dedicated tool listener; panels are read-only.

## Automatic worker compaction

A worker advertising `auto-compact` keeps one active request across task segments,
summary work and private continuation. The Hub waits for the single final
`run_finished`; `compact_finished` is not a delegated-request completion signal.
Its `origin: "automatic"` invalidates history without creating a manual compact
round. The paired host-generated `auto_compact` tool card reports progress and
needs no confirmation. Existing cancellation remains available throughout.

History turns marked `internal_input: "auto_compact_continue"` contain no user
text, but retain their input `source` and assistant responses. Each new history
step has its own `execution: {worker_id, request_id, run_id}` identifying the run
that committed it; later Continue requests and worker restarts do not rewrite the
turn's original input source. Panels hide the internal user bubble and match
execution plus decimal-string `commit_sequence` against replay. Missing responses
are restored inside their execution round while retaining its live tool cards;
responses with no matching execution remain standalone history. Event cursors
apply only to the queried worker incarnation, not prior workers' sequence numbers.
See [worker automatic compaction](../core/worker-protocol.md#automatic-context-compaction).

## Complete answer access and large output

`POST /api/sessions/:id/answer` accepts `{source, part, offset}`. It is authenticated
with the normal panel token, verifies the selected live session/worker capability,
and proxies one read-only worker query. Responses are correlated to that exact
connection and commit; disconnect, source expiration or invalid cursor returns
HTTP 409 `answer_unavailable`. Client disconnect cancels the query. At most 64
queries globally and two per worker are pending, with a ten-second deadline;
no complete-answer cache or unbounded pending queue is introduced.

The response is the worker `answer` data object documented in the
[Simplex Loop Worker Protocol](../core/worker-protocol.md#complete-answer-pages).
The panel offers explicit next-page navigation and copies only the displayed
page. Pages preserve every answer byte/part without mounting an entire large
Markdown document. Large inline answers use plain-text section navigation without discarding
the retained text; reasoning always uses lazy literal text, never Markdown/highlighting.

Headless conversation projections preserve `answer_source` in storage. A parent
may call `subagent_receive` with `subagent_id` and `answer: {source, part, offset}`
to read the exact child answer in 32 KiB pages. Turn pagination (`cursor`, `limit`)
and answer pagination are mutually exclusive. Direct-parent authorization is
rechecked after the awaited query; child stop/deletion and worker restart retain
the existing lifecycle semantics. Unknown/older workers offer explicit previews
without a working full-content promise. Resource eviction can shorten projections
but never silently rewrites the canonical worker answer.

Accepted worker display data is normalized before caching: answers receive an
aggregate 512 KiB encoded budget, reasoning a 4 KiB UTF-8 prefix; extras/unknown
metadata are bounded first. A normalized data envelope fits 768 KiB. Many fields,
JSON escapes and Unicode count toward the full frame. Transcript rings count
actual UTF-8 encoded bytes. A display entry above a configured ring budget becomes
an omission record retaining correlation and any answer source; if even that record
cannot fit, eviction reports a retention gap instead of repeatedly disconnecting. Ingress still
has its configured hard parser limit: oversized older-worker frames cannot be
recovered by normalization. Optional event JSONL files remain best effort.
Whole-response omission is displayed explicitly, rather than as an empty model
answer. When the omission record carries a valid `answer_source`, the panel keeps
the exact-answer pagination control available in both live display and replay.
Without a usable source it reports that full text is unavailable.

Tool calls/results use dedicated projections with independent per-entry argument,
output and metadata budgets. Call ID, name and classifications, result query
identity, status, `loop_skipped` and framework error fields do not share a traversal
budget with arbitrary argument/output trees. Proposals, approvals and returned
results therefore remain correlated when a body is only a preview. Up to 64 batch
entries are shown; any omitted suffix is counted explicitly.
Normalization preserves and accumulates existing `display_omitted` batch-marker
counts instead of counting a marker as one tool. The panel excludes these markers
from proposals/results and renders notices; the paired model proposal and
`tool_calls` event share one call-omission notice. Result omissions are independent
and never imply an anonymous successful execution.

Display truncation never alters executable payloads, tool arguments, approval
identities or decisions. Oversized indivisible execution/control messages are
rejected explicitly. An approval preview identifies omitted content; the original
pending operation remains authority. Browser transcript copies are byte bounded
per view and across inactive sessions; history/replay can restore evicted data.

### Display resource accounting

The panel retains up to 8 MiB of transcript JSON and 8 MiB of history JSON per
session, plus a 4 MiB/32-name derived event cache. Inactive views are evicted when
these display copies exceed 40 MiB across sessions; history eviction is indicated
and does not change worker persistence. Source-aware answer navigation keeps one
32 KiB page in the DOM instead of assembling an unbounded document. Large normal
answers use a plain-text rendering window of 32,768 UTF-16 code units with
surrogate-safe boundaries and explicit section navigation; reasoning always uses literal text.

Confirmation and remote-tool ingress each cap one executable frame at the lesser
of the configured ingress ceiling and 1 MiB. Oversized operations are rejected,
never rewritten into a clipped operation. Confirmation displays preview arguments
and extras, retain the original pending request as approval authority, and show
an explicit warning when the full review is unavailable. At most 64 approvals per
session are pending; further requests are denied as capacity exhausted. Remote
results retain their 256 KiB frame ceiling; answer pagination fits this ceiling.

Panel frames have a 2 MiB indivisible display ceiling beneath the 4 MiB socket
backlog limit. Approval previews share the aggregate allowance described above,
including on initialization, paged replay and reconnect. Modern replay is paged;
an oversized legacy bulk/session snapshot whose other contents cannot fit
returns `display_snapshot_too_large` on the live connection. Authenticated REST
session/log endpoints remain available for explicit retrieval. WebSocket
log snapshots use a 256 KiB newest tail, with 8 KiB line previews and explicit
omission notices. Their captured logs and execution inputs are not rewritten.
Worker correlation/event-name fields above 128 UTF-8 bytes are rejected rather
than cropped into another identity.
