# Hub UI SDK

**Panel protocol** `simplex-hub-panel`, version **1**. Use the public ESM
entry point `@hazer-bjtu/simplex-hub/ui` to build a browser panel, terminal
client or read-only session observer. It exports types and static constants.
Your client opens the transport, validates received JSON and maintains its view.

## Contract inventory

| Task | Public exports | How to use them |
| --- | --- | --- |
| Discover support | `PANEL_PROTOCOL`, `PANEL_VERSION`, `HubMetadata` | Read the remote protocol and capabilities from public `GET /api/meta` or the socket's `welcome.hub`. |
| Send or receive a frame | `PANEL_SOCKET_PATH`, `PanelMessage`, `HubMessage` | Use `/panel/ws` and the message's `type` discriminator. |
| Call REST operations | `REST_ROUTES`, `RestOperations`, `RestResponse` | Look up the method/path key for parameters, query, body and responses. Inspect HTTP status and operation outcome. |
| Follow a session | `SessionDescription`, `RequestRecord`, `WorkerEnvelope` | Correlate worker state, input admission and final outcome by session and request. |
| Handle operator interaction | `ConfirmationPrompt`, `ConfirmationOutcome`, `OperatorQuestion` | Route by session and prompt/question ID. Approvals reach every connected panel. |
| Recover displayed content | `HistoryPage`, `AnswerSource`, `AnswerQuery`, `AnswerPage` | Rebuild a conversation view or retrieve the full text behind an answer preview. |

Use a bearer token on protected HTTP routes. Browser clients can supply the
panel token as a query parameter when opening the socket. Keep it out of logs.
Serve browser clients from the Hub's public origin: a supplied WebSocket Origin
must match the request Host. [Wire vocabulary and REST reference](hub-protocol.md)

## What a client can observe

| Observation | Meaning |
| --- | --- |
| `accepted` | The Hub accepted the panel command. Worker admission can still fail. |
| `request` or worker `input_admitted` / `input_rejected` | The Hub reports request tracking or forwards the worker's admission decision. |
| Worker `run_finished` | The worker settled the request. A fatal error or lost connection can leave the outcome unknown. |
| `connection` with `connected: false` | The Hub lost its worker connection. The panel socket can still be live. |

| Read surface | Cursor and recovery rule |
| --- | --- |
| Hub transcript | Use the last received `hub_sequence` with its `transcript_epoch`. Start fresh when the epoch changes. A sequence gap means some display events are unavailable. |
| Worker display history | Match the query ID, worker instance, revision and turn/step cursor. Restart the read if the revision changes. |
| Complete answer | Keep the entire `AnswerSource`. Follow part indices and UTF-8 byte offsets. Refresh the source after replacement or worker restart. |

Use worker `sequence` only within its own process incarnation. On uncertain
delivery, reconcile state before explicitly retrying input or an approval.
[Worker protocol](../core/worker-protocol.md)

## Example: observe an ordinary session

This browser observer opens a connection to the current Hub origin and prints
retained and live event names for one session. Supply a decoder that validates
incoming JSON against the fields you consume and throws on malformed input.
Call the function again after disconnect to read a fresh, bounded replay.

```ts
import { PANEL_PROTOCOL, PANEL_SOCKET_PATH, PANEL_VERSION } from '@hazer-bjtu/simplex-hub/ui';
import type { HubMessage, PanelMessage, WorkerEnvelope } from '@hazer-bjtu/simplex-hub/ui';

export function observe(session: string, token: string, decode: (text: string) => HubMessage) {
    const url = new URL(PANEL_SOCKET_PATH, location.href);
    url.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    if (token) url.searchParams.set('token', token);
    const socket = new WebSocket(url);
    const request = crypto.randomUUID();
    let epoch: string | undefined;
    let cursor = 0;
    let live = false;

    function show(event: WorkerEnvelope) {
        const seq = event.hub_sequence;
        if (event.session_id !== session || seq === undefined || seq <= cursor) return;
        if (seq > cursor + 1) console.warn('Transcript gap');
        console.log(seq, event.event);
        cursor = seq;
    }

    socket.onmessage = ({ data }) => {
        try {
            if (typeof data !== 'string') throw new Error();
            const msg = decode(data);
            if (msg.type === 'welcome') {
                const hub = msg.hub;
                if (hub.protocol.name !== PANEL_PROTOCOL.name || hub.protocol.version !== PANEL_VERSION
                    || !hub.capabilities.includes('transcript-pages')
                    || !hub.capabilities.includes('transcript-epoch') || !hub.transcript_epoch) {
                    throw new Error();
                }
                epoch = hub.transcript_epoch;
                const sub: PanelMessage = { v: PANEL_VERSION, type: 'subscribe', session,
                    request_id: request, paged: true, replace: true, since: 0 };
                socket.send(JSON.stringify(sub));
            } else if (msg.type === 'subscribed' && msg.request_id === request
                       && msg.session.session_id === session) {
                if (!epoch || msg.transcript_epoch !== epoch) throw new Error();
                for (const event of msg.transcript) show(event);
                if (msg.latest > cursor) console.warn('Transcript gap');
                cursor = msg.latest;
                live = msg.replay_more === false;
            } else if (msg.type === 'event' && live && msg.session === session) {
                show(msg.envelope);
            } else if (msg.type === 'error') {
                console.warn(msg.error);
                socket.close();
            }
        } catch {
            socket.close(4000, 'Invalid or unsupported stream');
        }
    };
    socket.onclose = () => console.info('Observer disconnected');
    return socket;
}
```

The Hub sends all pages for one subscription. The final `subscribed` frame
sets `replay_more: false`, then live session events follow. A graphical client
replaces its prior display on `replay_reset` and bounds its retained view.
[Runnable browser and TUI examples](https://github.com/Hazer-BJTU/simplex-cpp/tree/main/hub/examples/ui-sdk)
