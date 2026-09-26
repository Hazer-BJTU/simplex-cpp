# Hub panel protocol, version 1

The hub's own client protocol: what a browser (or any operator tool) exchanges
with the hub to manage sessions, read worker events, answer confirmations, and
control worker processes.

This is **not** the worker protocol. The worker-facing contract stays in
[`core/docs/worker-protocol.md`](../../core/docs/worker-protocol.md); a browser
never speaks it, because the worker-facing payload channel is an approval
authority and must stay a deployment-trusted endpoint. How the hub implements
the worker side is described in [worker-adapter.md](worker-adapter.md).

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

### Capabilities

| Capability | Meaning |
| --- | --- |
| `worker-events` | worker envelopes are forwarded, unknown event names included |
| `confirmations` | tool confirmations are surfaced and can be answered |
| `supervisor` | worker processes can be started, stopped, and force-killed |
| `transcript-replay` | `subscribed` replays the transcript after a cursor |
| `snapshot-view` | the worker's persisted snapshot can be read, never written |
| `transcript-epoch` | `transcript_epoch` is reported, so a stale cursor is detectable |
| `global-confirmations` | confirmations reach every client, not only subscribers |
| `session-history` | panel can query a worker's simplified conversation history |

These describe the hub **build**, not its configuration. `supervisor` means
"this hub starts and signals worker processes", which stays true whichever
launcher renders the configuration; the launcher's own difference is reported
separately as `launcher.owns_config`. A capability that varied with
configuration would be a different kind of list, and nothing in this one does.

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

Most messages embed this object, produced by `Session.describe()`:

```json
{
  "session_id": "demo",
  "created_at": "2026-01-01T00:00:00.000Z",
  "spec": {
    "provider": "mock", "model": "", "threads": 1, "maxExchanges": 512,
    "systemPromptFile": "/abs/path/coding_agent.yaml",
    "workspace": "", "platform": "", "software": [],
    "persistence": {"enabled": true, "readable": false}, "restore": "if_present",
    "env": {}, "extraArgs": []
  },
  "connected": true,
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
  "process_group_killed": false, "log_path": "...", "log_lines": 12, "log_dropped": 0
}
```

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

## JSON API

| Method and path | Body | Response |
| --- | --- | --- |
| `GET /api/meta` | — | hub metadata and capabilities |
| `GET /api/sessions` | — | `{sessions: [session...]}` |
| `POST /api/sessions` | `{session, spec?}` | `201 {session}`; `400 invalid_session`; `409 session_exists` |
| `GET /api/sessions/:id` | — | `{session}`; `404 unknown_session` |
| `DELETE /api/sessions/:id` | — | `{removed}`; `409 session_busy` while a worker runs or is connected |
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
`hub_sequence` counts envelopes received by *this hub process*, which is what a
client resumes from. `latest` is the current end of the transcript.

`snapshot` reads the worker's own files
(`<persistence.directory>/<session>/state.json` and `readable.md`) without
modifying them. The worker owns that state; there is no operation to replace,
edit, or reset it. Files larger than 8 MiB are skipped rather than streamed.

## Panel WebSocket

### Client to hub

| Message | Fields | Effect |
| --- | --- | --- |
| `subscribe` | `session`, optional `since` | sends `subscribed` with the transcript after `since` |
| `unsubscribe` | `session` | stops live messages for that session |
| `list_sessions` | — | answers with `sessions` |
| `create_session` | `session`, optional `spec` | answers with `created`, or `session_exists` / `invalid_session` |
| `delete_session` | `session` | answers with `session_removed`; refused while busy |
| `worker` | `session`, `action`: `start`\|`stop`\|`restart`\|`force-kill`, optional `spec` | answers with `accepted` (carrying the result) or `worker_action_failed` |
| `input` | `session`, `content`, optional `operation`, `request_id`, `options` | validates, sends a payload, answers with `accepted` and `request_id` |
| `history` | `session`, optional `request_id`, `start`, `step`, `limit` | sends a read-only worker history payload; response arrives as a `history` worker event |
| `signal` | `session`, `operation`: `status`\|`options`\|`cancel`\|`shutdown`, optional `run_id` | answers with `accepted` or `signal_not_sent` |
| `confirmation` | `session`, `confirmation_id`, `decision`, optional `reason` | answers with `accepted` or `confirmation_rejected` |
| `logs` | `session`, optional `limit` | answers with up to 2000 captured worker lines |
| `status_snapshot` | `session`, optional `since` | answers with a fresh `snapshot` |
| `ping` | — | answers with `pong` |

`input` accepts the same content parts as the worker protocol (`text`,
`binary`, `external_ref` with optional `extras`) and the same option categories
(`model`, `tools` — reserved and empty, `confirmation.mode`). The hub validates
them locally so the panel can report a mistake immediately; the worker is still
authoritative and its rejection is surfaced unchanged.

`signal` with `operation: "cancel"` defaults `run_id` to the most recently
observed one. A stale id is ignored by the worker, so the default is convenient
rather than dangerous.

`history` pages are a bounded display projection of the worker's in-memory
`UserLoopStep` turns. The browser requests pages in order when it subscribes
and refreshes after `run_finished`, but only after the current worker advertises
`session-history` in a `ready` or `status` event. The hub's own capability means
it can route these requests; it does not imply that an older worker can answer.
The browser restarts pagination if the worker's
history revision changes between pages. The hub forwards each full response
live, but retains only a small cursor marker in its transcript and JSONL log.
The marker has `transient_history: true` and omits `turns` and the raw document;
it preserves sequence positions for replay without duplicating conversation
content in hub storage.
The authoritative restorable history remains the worker's `state.json`.
An offline worker cannot answer a live history query; the panel keeps its last
displayed page and offers a refresh after reconnection.

### Hub to client

| Message | Fields | Meaning |
| --- | --- | --- |
| `welcome` | `hub`, `sessions` | sent once per connection |
| `sessions` | `sessions` | full list, on request |
| `session` | `session` | one session changed |
| `session_removed` | `session` | deleted |
| `subscribed` | `session`, `transcript`, `logs`, `latest`, `transcript_epoch` | subscription accepted, with replay |
| `created` | `session` | session created by this client |
| `event` | `session`, `hub_seq`, `envelope` | one worker event, verbatim |
| `confirmation` | `session`, `open`, `confirmation`, and `outcome` when closing | prompt opened or retired/answered; sent to **every** connected panel, not only subscribers of that session |
| `process` | `session`, `process` | worker process state changed |
| `connection` | `session`, `connected`, `identity` | event connection opened or closed |
| `request` | `session`, `request` | payload outcome changed |
| `logs` | `session`, `lines`, `dropped` | log tail, only in response to `logs` |
| `snapshot` | `session`, `transcript` | transcript plus description, on request |
| `accepted` | `action`, `session`, plus action-specific fields | the command was accepted |
| `error` | `error`, `message`, optional `request` | the command was refused |
| `pong` | `at` | heartbeat reply |

`envelope` is the worker's envelope with hub-added fields:
`hub_sequence`, `received_at`, `known`, `issues`, `raw` (the untouched document
as received). Unknown event names are forwarded exactly like known ones; the
panel decides how to render them.
The sole replay exception is a `history` response: live subscribers receive its
full envelope, while replay contains the small `transient_history` marker
described above. A replay marker cannot reconstruct a history page; clients
must issue a fresh `history` query.

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

### Replay cursors and the transcript epoch

`hub_sequence` counts envelopes received by *this hub process*, so it starts
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
| `input_not_sent` | validation failed or the worker is not connected |
| `signal_not_sent` | same, for signals |
| `unknown_confirmation` | that prompt is no longer open |
| `confirmation_rejected` | the decision was refused (already answered, or identity unverified) |
| `worker_action_failed` | the launcher or supervisor refused the action; `result.error` explains |
| `binary_not_supported` | panel messages must be text |
| `internal_error` | the hub failed to process the message; it is still serving |

## Trust boundary

Anyone who can reach the panel can submit payloads, and a payload may select
`confirmation.mode: approve`, which is equivalent to approving every tool call
that requires confirmation. The hub therefore:

- defaults to a loopback listener, and refuses a non-loopback one without a
  panel token,
- refuses cross-origin WebSocket upgrades,
- keeps the worker-facing channel separate, with its own per-session tokens,
- validates and refuses malformed or oversized requests instead of forwarding
  them.

It does **not** implement user accounts, roles, audit logs, or TLS. Deployments
that need those should terminate TLS and authenticate at a reverse proxy, and
keep the hub on a private interface.
