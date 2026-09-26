# Worker adapter

How this hub implements the worker-facing contract, and what it deliberately
leaves to the deployment.

[`core/docs/worker-protocol.md`](../../core/docs/worker-protocol.md) is the
authoritative wire contract. This page does not restate it: it records which
module implements each part, which test covers it, where behaviour is
deliberately different, and what is missing. When the two disagree, the core
document wins and this page is wrong.

## Routes and binding

| Route | Role | Lifetime | Implemented by |
| --- | --- | --- | --- |
| `GET /agent/<session_id>/events?token=<t>` | events and inputs | persistent, reconnecting | `src/worker/connection.ts` |
| `GET /agent/<session_id>/confirm?token=<t>` | one-shot confirmation | one request, one response, then close | `src/worker/confirmation.ts` |
| anything else | — | rejected during the upgrade | `src/http/server.ts` |

Both are plain `ws://`. A separate `GET /api/*` + `/panel/ws` surface serves the
browser; the worker never speaks it.

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

## Identity

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

## Confirmation judgement

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

## Events

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
  error counter. Twenty fatal errors close the connection (1008). Non-fatal
  issues (missing `request_id`, `run_id`, `sequence`, or `data`) are recorded on
  the envelope and the event is still surfaced.
- A WebSocket ping runs every `limits.pingIntervalMs` (default 30 s) and a
  missed pong terminates the socket, so a half-open connection stops looking
  like a live worker.

### A second event connection supersedes the first

`simplex_shell` answers a second event connection with HTTP 409. This hub does
not: it closes the previous connection (code 4001) and accepts the new one.

The reason is operational. A worker that reconnects after an unobserved peer
death is the common case, the worker retries a rejected upgrade forever anyway
(with capped backoff and no attempt limit), and two live workers on one session
are already prevented by the worker's own session lock. Rejecting the upgrade
would leave the hub unable to talk to the worker that is actually running. The
replacement is logged at warn level and the panel shows the new connection.

## Inputs and signals

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

## Process lifecycle

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

## Persistence

The hub stores sessions, launch specs, tokens, and process identity in
`hub.json`. Conversation history is deliberately absent — that is the worker's
own snapshot, and duplicating it would create a second source of truth. The
panel can *read* `<persistence.directory>/<session>/state.json` and
`readable.md`; there is no operation to replace, edit, or reset a snapshot,
because the protocol defines none and inventing one would need a managed
integration on the worker side.

The hub's own event transcript (`<dataDir>/events/<session>.jsonl`) is an
operator artifact. A hub restart starts an empty in-memory transcript instead of
replaying the file, so panel replay is scoped to one hub process.

## Requirement-by-requirement

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

## Known limits

- No delivery acknowledgement exists, so the hub cannot prove that a payload was
  admitted, executed, or persisted. It shows what it observed and marks the
  rest unknown.
- The hub's in-memory transcript does not survive a restart; the JSONL log does,
  but it is not replayed into the panel.
- A confirmation's local deadline is advisory, as explained above.
- Cancellation does not roll back accepted tool calls; the hub says so in the UI
  rather than implying otherwise.
- `wss://` is not implemented. Terminate TLS at a reverse proxy.
- The panel has no user accounts or roles. One optional shared token guards the
  browser surface; anyone who can reach the panel can approve tool calls.
- Orphan detection for a worker started outside the hub is limited to what the
  protocol allows: the hub can drive and stop it, but it has no process record
  and the panel marks it as unattached.
