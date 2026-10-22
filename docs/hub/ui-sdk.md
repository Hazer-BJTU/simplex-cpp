# UI SDK and independent clients

The ESM entry point `@hazer-bjtu/simplex-hub/ui` exposes the Hub's panel protocol
as TypeScript declarations and static constants. It supports independently
implemented terminal and Web clients without adopting the bundled panel's
architecture. Availability begins with the release containing the SDK changes;
this documentation tracks `main`, so earlier npm versions may not export it.

```ts
import { PANEL_PROTOCOL, PANEL_VERSION, PANEL_SOCKET_PATH }
    from '@hazer-bjtu/simplex-hub/ui';
import type { PanelMessage, HubMessage, RestOperations, AnswerPage }
    from '@hazer-bjtu/simplex-hub/ui';

const subscription: PanelMessage = {
    v: PANEL_VERSION,
    type: 'subscribe',
    session: 'example',
    since: 0,
    paged: true,
    replace: true,
    request_id: 'subscription-1',
};
type CreateSession = RestOperations['POST /api/sessions']['body'];
```

There are no exported clients, transports, validators, stores, UI components,
URL builders or automatic recovery functions. Types do not validate untrusted
JSON. Consumers own authentication, validation, request correlation, state,
rendering, replay, pagination and user interaction. Only `./ui` is a public
package API; the package root and private deep imports are not supported.
Installing the combined package also installs the Hub's production dependencies,
but an SDK import does not load or require them. There is no CommonJS SDK entry.

## Contract inventory

| Contract family | Public names and purpose |
| --- | --- |
| Discovery | `PANEL_PROTOCOL`, `PANEL_VERSION`, `CAPABILITIES`, `HubMetadata` |
| Socket vocabulary | `PANEL_SOCKET_PATH`, `PanelMessage`, `HubMessage`, message type arrays, accepted/error records |
| Sessions | `SessionDescription`, `SessionIdentity`, `SessionSpec`, `ProcessDescription`, `SubagentDescription`, request/admission records |
| Approvals | `ConfirmationPrompt`, `ConfirmationOutcome`, `PendingCall` |
| Content | `ContentPart`, `WorkerEnvelope`, `HistoryPage`, `HistoryTurn`, `ExecutionIdentity`, `SessionPlan` |
| Exact answers | `AnswerSource`, `AnswerQuery`, `AnswerPage` |
| Configuration library | Configuration text/revision, template, selection, save/rename/remove/validate and endpoint-preview DTOs |
| REST | `REST_ROUTES`, `RestOperations`, `RestOperation`, `RestResponse`, supervisor/policy/recovery DTOs |

The REST table covers every panel HTTP method/path. Each operation declares
`params`, `query`, `body`, status-indexed `responses` and the common `error`
envelope. `never` means that request component is unused. Numeric query values
need URL serialization by the consumer. HTTP success is not necessarily
operation success: start/restart can return supervisor refusals at 409,
recovery can return a failed stop result at 409, and force-kill can return
`ok: false` at 200. Common errors can occur at other statuses too. Error strings
remain open; `ERROR_CODES` is not an exhaustive REST error catalog.

[Hub panel protocol](./hub-protocol.md), [configuration library](./configurations.md)
and [headless subagents](./subagents.md) remain the wire authorities. The SDK
source includes API comments in emitted declarations; its contributor guide
explains how to maintain one canonical declaration per field.

## Authentication and deployment

`GET /api/meta` is public. Other management REST routes require operator
credentials when a panel token is configured. Use `Authorization: Bearer ...`
for HTTP. Browser WebSockets cannot set that header: supply the token as the
`token` query parameter on `/panel/ws`. Keep tokens in memory, use HTTPS/WSS
outside trusted loopback networks, and redact query strings from proxy logs.
Configuration text and launch descriptions may contain secrets; never copy them
into unrelated logs or services.

A non-browser WebSocket without an Origin header is supported. An explicitly
supplied Origin must have the same host as the request Host. There is no general
cross-origin REST/CORS support. Serve a separate browser application behind the
same public origin and forward `/api` and `/panel/ws`, including WebSocket
upgrades. Do not disable token or Origin checks.

For example, serve the built example under `/sdk/` and forward the Hub API:

```nginx
# Inside a server block with TLS configured as appropriate.
root /srv/simplex-examples;
location /sdk/ { try_files $uri $uri/ =404; }
location /api/ {
    proxy_pass http://127.0.0.1:8787;
    proxy_set_header Host $http_host;
}
location /panel/ws {
    proxy_pass http://127.0.0.1:8787;
    proxy_http_version 1.1;
    proxy_set_header Host $http_host;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    # The browser Origin is forwarded unchanged and checked against Host.
    access_log off;
}
```

The original request path is retained. Copy `web/dist/` contents to
`/srv/simplex-examples/sdk/`. Preserve the public Host including its port.
Avoid proxy redirects on `/panel/ws`; align the upstream port with your Hub.

## Compatibility and observation boundaries

Read protocol version and capabilities from the remote `/api/meta` and
`welcome`. The installed npm version identifies local SDK declarations, not
remote feature support. Optional legacy fields may be absent; tolerate unknown
fields and open event/error strings while validating fields you actually use.
Do not treat an unknown event name as permission to interpret its body.

Distinguish these observations:

1. A local socket send completes: bytes were queued or written locally.
2. A Hub `accepted` response confirms the Hub accepted that command.
3. A `RequestRecord` reports `sent`, worker `admitted`, `rejected`, or `unknown`.
4. Worker events report the actual run and its final `run_finished` result.

None of the earlier observations proves the later ones. After uncertain
delivery, never automatically resend user input, confirmation decisions or
destructive lifecycle/recovery actions. Give the operator an explicit choice.
Ordinary and headless sessions have different controls: headless sessions have
no direct panel conversation, remain operator-approvable, and disappear from the
list after successful cleanup. Parent workers cannot elevate a child's safety
policy. Cleanup recovery and termination attestations are operator authority,
not ordinary worker commands. Lifecycle IDs protect against acting on a
replacement process; displayed paths/PIDs are diagnostics, not local authority.

## Transcript, history and answer recovery

The three read surfaces have different owners and cursors:

- **Hub transcript:** a bounded event projection. `hub_sequence` belongs to one
  `transcript_epoch` and is unrelated to the worker's event `sequence`. Reset
  local transcript cursors when the epoch changes.
- **Worker history:** display-only turns/exchanges, not `AgentInputState`.
  Preserve the revision and both turn/step cursor components while paging;
  discard a refresh when its revision or correlation changes.
- **Exact answers:** committed content identified by worker ID, turn, step,
  decimal commit sequence and fingerprint. Preserve the whole `AnswerSource`.
  `part` indexes content parts; `offset` and `next_offset` count UTF-8 bytes,
  not UTF-16 string length. Verify forward progress, advance parts correctly,
  and never combine pages from different sources.

For transcript pages, use a new `request_id` for each subscription. Ignore
superseded replies. `replay_reset` establishes replacement rather than a delta;
`replay_more` means the replay is not yet complete. Treat sequence gaps and
retention omissions as incomplete history, not an empty successful recovery.
A slow client may be disconnected for backpressure: reconnect and request a
bounded replay rather than assuming every live frame was retained. Do not
accumulate an unbounded display cache.

Panel↔Hub disconnect and Hub↔Worker disconnect are distinct. On either boundary,
clear relevant in-flight operations; on worker reconnect rediscover its identity
and capabilities. A still-connected panel does not prove a still-connected
worker. Reconnection must not silently retry a command whose delivery is unknown.

## Confirmations and compaction

Approval prompts are global, not confined to the selected session. Correlate
using session, confirmation, worker and run identity, and reconcile pending
submissions against fresh snapshots after reconnect. A preview can be truncated
or omitted; it is not the executable argument object. Preserve truncation and
original-size metadata, accept richer snapshot previews, and show uncertainty
while a decision is pending. Do not interpret a disappearing prompt alone as an
approval you submitted successfully.

Manual and automatic compaction can invalidate worker history. Keep internal
`auto_compact_continue` inputs out of visible user messages. Automatic compaction
can include several internal runs but ends with one final completion for the
original accepted input. A compaction progress event is not that completion.
A complete client should reconcile history after compaction and use committed
answer pagination for content that exceeds display projections.

## Read-only examples

The repository contains `hub/examples/ui-sdk/tui/` and `web/`. Both import the
**packed public subpath**, own their presentation/transport, and share a small
example-only observer outside the SDK. They discover ordinary sessions, request
correlated paged replay from zero and show live event labels. Reconnect is
explicit and starts a fresh projection; a retention gap or the 200-entry local
preview bound is visibly marked incomplete.

They intentionally do not send tasks, approve tools, control workers, fetch
worker history/exact answers, render message content, or implement automatic
reconnect/retry. They require `transcript-pages` and `transcript-epoch` and report
unsupported peers instead of pretending replay is complete. They are teaching
examples, not a production client runtime or alternative built-in panel.

From a source checkout, pack the current implementation and install it in a
separate example directory (older registry releases may not have the SDK):

```bash
npm --prefix hub ci --ignore-scripts
npm --prefix hub run build:release
mkdir -p /tmp/simplex-sdk-pack
(cd hub && npm pack --ignore-scripts --pack-destination /tmp/simplex-sdk-pack)
cp -R hub/examples/ui-sdk /tmp/simplex-sdk-demo
cd /tmp/simplex-sdk-demo
# Replace VERSION with the archive version printed by npm pack.
npm install --ignore-scripts /tmp/simplex-sdk-pack/hazer-bjtu-simplex-hub-VERSION.tgz
read -rs SIMPLEX_PANEL_TOKEN
export SIMPLEX_PANEL_TOKEN
node tui/main.mjs http://127.0.0.1:8787            # list sessions
node tui/main.mjs http://127.0.0.1:8787 SESSION_ID # observe one
```

For the Web example, install the same Vite version pinned in `hub/package-lock.json`
as a development dependency in that demo directory, then run
`npx vite build web --base=./`. Deploy `web/dist/` behind the proxy above, open
`/sdk/`, enter the operator token and choose a session. The token is not saved.
Do not expose Vite's development server as a production authentication boundary.

Packed-package validation builds the Web example and exercises both adapters
against a real Hub and controlled fixture worker: multi-page replay, retention
gaps, authentication, an actual same-origin proxy, restart/epoch change, and
unsupported capabilities. The bundled panel retains its own richer recovery
logic; its imports and behavior remain separately tested.


## Optional panel decoration

`HubMetadata.background` is an optional `PanelBackground` descriptor containing
`url`, a SHA-256 `revision`, `size`, `position`, light/dark `opacity`, and the
optional light/dark `toolCardOpacity` (absent from older Hubs; 0.6 when omitted).
It never contains a local file path. Clients may ignore it.

`GET /api/panel/background/image` returns PNG/JPEG bytes, not JSON;
`HEAD` returns the same headers without a body. Both use normal panel
authentication (send the bearer credential in the header, never the URL).
Responses use `Cache-Control: private, no-cache`, `Vary: Authorization`,
`X-Content-Type-Options: nosniff`, and a quoted revision ETag. Authorized
conditional requests can return 304; an unconfigured/unavailable image returns
404. The endpoint has no file-selection parameter.

The bundled panel fetches and decodes a bounded blob, revokes its object URL on
replacement/disconnection, and treats image errors as decoration failures.
Its CSP permits `blob:` for images only. Theme changes do not download the image
again. Restart Hub to publish a new snapshot or appearance settings.
