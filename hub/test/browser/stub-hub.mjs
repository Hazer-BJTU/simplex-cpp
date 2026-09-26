/**
 * @file a scripted hub for the browser tests.
 *
 * The panel's behaviour depends on things a real hub does over time: it replays
 * a delta on subscribe, it can restart (which changes the transcript epoch), and
 * it can raise an approval for a session the panel is not watching. Reproducing
 * those against a real hub means a real worker and a real model, which is slow,
 * needs the C++ build, and cannot be told "now pretend you restarted".
 *
 * So this is the smallest server that speaks the *panel* protocol honestly: the
 * same routes, the same message shapes, the same version constant — imported
 * from `shared/protocol.ts` rather than repeated, so the stub cannot drift into
 * agreeing with a panel that is wrong. It has no worker protocol side at all:
 * envelopes are injected by the test through `/__stub/*`.
 *
 * It is not a mock of the hub's logic. Ordering, cursors and epochs are real
 * here because those are exactly what the tests are about.
 */
import { createServer } from 'node:http';
import { WebSocketServer } from 'ws';
import { PANEL_VERSION } from '../../shared/protocol.ts';

const PORT = Number(process.env.STUB_HUB_PORT ?? 4180);
/** Identifies this stub's transcript series; `/__stub/restart` changes it. */
let epoch = 'stub-epoch-1';

/**
 * What `/api/meta` reports, so a test can change the configuration the panel
 * describes consequences from. `force_kill_process_group` is the one that
 * matters: the old panel's force-kill confirmation claimed it unconditionally.
 */
let settings = { force_kill_process_group: false };

/** One session description, as `describe()` builds it on the real hub. */
function makeSession(id, createdAt = '2026-01-01T00:00:00.000Z') {
    return {
        session_id: id,
        created_at: createdAt,
        spec: {},
        connected: true,
        identity: { state: 'live', worker_id: 'stub-worker', since: null },
        stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 1 },
        last_run_id: '',
        last_event_at: null,
        last_event: null,
        confirmations: [],
        process: null,
        requests: [],
    };
}

/** Sessions the stub lists after a reset. */
const DEFAULT_SESSIONS = [makeSession('demo')];

/** Sessions the stub lists, replaced wholesale by a test when it needs to. */
let sessions = DEFAULT_SESSIONS;

/** session_id -> envelopes, each already carrying `hub_sequence`. */
const transcripts = new Map();

/** Open panel sockets. */
const panels = new Set();

/** Everything panels sent, so a test can assert what the panel asked for. */
const received = [];

/** How long a snapshot answer takes; a test sets it to create a race. */
let payloadDelay = 0;

/**
 * When true, panel upgrades are refused and existing sockets closed.
 *
 * Taking the socket away is not something `page.route` can do — a WebSocket
 * upgrade is not an HTTP request it intercepts — so the hub has to be the one
 * to go down, which is also closer to what actually happens.
 */
let down = false;

/** The next hub sequence to hand out. */
let sequence = 0;

function sessionOf(id) {
    return sessions.find((session) => session.session_id === id) ?? null;
}

function describe(id) {
    return sessionOf(id) ?? {
        ...sessions[0],
        session_id: id,
        created_at: new Date().toISOString(),
    };
}

function transcriptOf(id) {
    if (!transcripts.has(id)) transcripts.set(id, []);
    return transcripts.get(id);
}

function meta() {
    return {
        name: 'stub-hub',
        version: '0.0.0',
        protocol: { name: 'simplex-hub-panel', version: PANEL_VERSION },
        worker_protocol: 'stub',
        capabilities: [
            'worker-events', 'confirmations', 'supervisor', 'transcript-replay',
            'snapshot-view', 'transcript-epoch', 'global-confirmations',
        ],
        transcript_epoch: epoch,
        listen: { host: '127.0.0.1', port: PORT },
        launcher: { kind: 'local', owns_config: true },
        provider_profiles: [],
        force_kill_process_group: settings.force_kill_process_group,
        mock: { enabled: true },
    };
}

function send(socket, message) {
    socket.send(JSON.stringify({ v: PANEL_VERSION, ...message }));
}

function broadcast(message) {
    for (const socket of panels) send(socket, message);
}

/** Read a JSON request body. */
async function body(req) {
    const chunks = [];
    for await (const chunk of req) chunks.push(chunk);
    const text = Buffer.concat(chunks).toString('utf8');
    if (!text) return {};
    try {
        return JSON.parse(text);
    } catch {
        return {};
    }
}

function json(res, status, payload) {
    const text = JSON.stringify(payload);
    res.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) });
    res.end(text);
}

/** Append one envelope and return it with its hub sequence applied. */
function append(sessionId, event, data, extra = {}) {
    sequence += 1;
    const envelope = {
        type: 'event',
        event,
        session_id: sessionId,
        worker_id: 'stub-worker',
        request_id: extra.request_id ?? 'stub-request',
        run_id: extra.run_id ?? 'stub-run',
        sequence: extra.sequence ?? sequence,
        data,
        hub_sequence: sequence,
        received_at: new Date().toISOString(),
        ...extra,
    };
    transcriptOf(sessionId).push(envelope);
    return envelope;
}

const server = createServer((req, res) => {
    const url = new URL(req.url ?? '/', `http://127.0.0.1:${PORT}`);

    // ------------------------------------------------------------ control --
    if (url.pathname.startsWith('/__stub/')) {
        void (async () => {
            const payload = await body(req);
            if (url.pathname === '/__stub/snapshot-delay') {
                payloadDelay = Number(payload.ms) || 0;
                json(res, 200, { ok: true, payloadDelay });
                return;
            }
            switch (url.pathname) {
                case '/__stub/reset': {
                    sequence = 0;
                    transcripts.clear();
                    received.length = 0;
                    epoch = 'stub-epoch-1';
                    settings = { force_kill_process_group: false };
                    down = false;
                    sessions = DEFAULT_SESSIONS.map((session) => ({
                        ...session, confirmations: [], requests: [],
                    }));
                    json(res, 200, { ok: true });
                    return;
                }
                case '/__stub/sessions': {
                    sessions = payload.sessions ?? sessions;
                    json(res, 200, { ok: true });
                    return;
                }
                case '/__stub/emit': {
                    const envelope = append(
                        payload.session ?? 'demo', payload.event ?? 'model_response',
                        payload.data ?? {}, payload.extra ?? {},
                    );
                    broadcast({
                        type: 'event',
                        session: envelope.session_id,
                        hub_seq: envelope.hub_sequence,
                        envelope,
                    });
                    json(res, 200, envelope);
                    return;
                }
                case '/__stub/confirm': {
                    const prompt = {
                        confirmation_id: payload.confirmation_id ?? 'c-1',
                        session_id: payload.session ?? 'demo',
                        worker_id: 'stub-worker',
                        run_id: 'stub-run',
                        state: 'awaiting-decision',
                        verified: payload.verified ?? true,
                        identity_state: 'live',
                        call: payload.call ?? { name: 'run_command', arguments: { command: 'ls' } },
                        received_at: new Date().toISOString(),
                        deadline_at: null,
                        settled_at: null,
                        decision: null,
                        reason: null,
                    };
                    // Recorded on the description too, because that is where the
                    // hub really keeps open prompts.
                    for (const session of sessions) {
                        if (session.session_id === prompt.session_id) {
                            session.confirmations = [...session.confirmations, prompt];
                        }
                    }
                    broadcast({
                        type: 'confirmation', session: prompt.session_id, open: true,
                        confirmation: prompt,
                    });
                    json(res, 200, prompt);
                    return;
                }
                case '/__stub/settle': {
                    for (const session of sessions) {
                        session.confirmations = session.confirmations.filter(
                            (prompt) => prompt.confirmation_id !== payload.confirmation_id,
                        );
                    }
                    broadcast({
                        type: 'confirmation',
                        session: payload.session ?? 'demo',
                        open: false,
                        confirmation: {
                            confirmation_id: payload.confirmation_id ?? 'c-1',
                            session_id: payload.session ?? 'demo',
                            worker_id: 'stub-worker', run_id: 'stub-run',
                            state: 'decided', verified: true, identity_state: 'live',
                            call: {}, received_at: new Date().toISOString(),
                            deadline_at: null, settled_at: new Date().toISOString(),
                            decision: payload.decision ?? 'approved', reason: null,
                        },
                        outcome: { phase: 'decided' },
                    });
                    json(res, 200, { ok: true });
                    return;
                }
                case '/__stub/restart': {
                    // A new hub process: same everything, a new epoch, and a
                    // numbering that starts again at 1.
                    epoch = payload.epoch ?? `stub-epoch-${Date.now()}`;
                    sequence = 0;
                    transcripts.clear();
                    for (const socket of panels) socket.close();
                    json(res, 200, { ok: true, epoch });
                    return;
                }
                case '/__stub/down': {
                    down = true;
                    for (const socket of panels) socket.close();
                    json(res, 200, { ok: true });
                    return;
                }
                case '/__stub/up': {
                    down = false;
                    json(res, 200, { ok: true });
                    return;
                }
                case '/__stub/settings': {
                    settings = { ...settings, ...payload };
                    json(res, 200, settings);
                    return;
                }
                case '/__stub/received': {
                    json(res, 200, { received });
                    return;
                }
                case '/__stub/decisions': {
                    // Confirmations the panel sent. The stub deliberately never
                    // answers them, which is what a refused or lost decision
                    // looks like from inside the panel — the case that used to
                    // disable its buttons forever.
                    json(res, 200, {
                        decisions: received.filter((message) => message.type === 'confirmation'),
                    });
                    return;
                }
                default:
                    json(res, 404, { error: 'unknown_control' });
            }
        })().catch((error) => json(res, 500, { error: String(error) }));
        return;
    }

    // --------------------------------------------------------------- api --
    if (url.pathname === '/api/meta') {
        json(res, 200, meta());
        return;
    }
    if (url.pathname === '/api/sessions') {
        json(res, 200, { sessions });
        return;
    }
    const snapshot = /^\/api\/sessions\/([^/]+)\/snapshot$/.exec(url.pathname);
    if (snapshot) {
        // Deliberately slow, so a test can switch sessions while the fetch is
        // in flight — which is the race the old snapshot pane lost.
        const id = decodeURIComponent(snapshot[1]);
        setTimeout(() => json(res, 200, {
            session_id: id,
            state: { session_id: id, marker: `state for ${id}` },
            readable: `# readable for ${id}`,
            files: { state: `/tmp/${id}/state.json` },
        }), Number(payloadDelay));
        return;
    }
    const events = /^\/api\/sessions\/([^/]+)\/events$/.exec(url.pathname);
    if (events) {
        const id = decodeURIComponent(events[1]);
        json(res, 200, {
            session: id,
            since: 0,
            latest: transcriptOf(id).at(-1)?.hub_sequence ?? 0,
            events: transcriptOf(id),
        });
        return;
    }
    const match = /^\/api\/sessions\/([^/]+)$/.exec(url.pathname);
    if (match) {
        json(res, 200, { session: describe(decodeURIComponent(match[1])) });
        return;
    }
    json(res, 404, { error: 'unknown_session', message: 'the stub has no such route' });
});

const wss = new WebSocketServer({ noServer: true });

server.on('upgrade', (req, socket, head) => {
    const url = new URL(req.url ?? '/', `http://127.0.0.1:${PORT}`);
    if (url.pathname !== '/panel/ws' || down) {
        socket.destroy();
        return;
    }
    // The same rule the real hub enforces, and for the same reason: a page the
    // operator visits must not be able to drive a hub on loopback just because
    // it can reach it. It is repeated here so that a proxy misconfiguration
    // fails in these tests rather than only against a real hub — `changeOrigin`
    // in the Vite proxy config is exactly that mistake, and a stub that skipped
    // this check would call it a pass.
    const origin = req.headers.origin;
    if (typeof origin === 'string' && origin.length > 0) {
        let originHost = null;
        try {
            originHost = new URL(origin).host;
        } catch {
            originHost = null;
        }
        if (originHost !== req.headers.host) {
            socket.write('HTTP/1.1 403 Forbidden\r\nConnection: close\r\n'
                + 'Content-Length: 0\r\n\r\n');
            socket.destroy();
            return;
        }
    }
    wss.handleUpgrade(req, socket, head, (ws) => {
        panels.add(ws);
        ws.on('close', () => panels.delete(ws));
        ws.on('message', (raw) => {
            let message = null;
            try {
                message = JSON.parse(raw.toString());
            } catch {
                return;
            }
            received.push(message);
            handle(ws, message);
        });
        send(ws, { type: 'welcome', hub: meta(), sessions, subscriptions: [] });
    });
});

function handle(ws, message) {
    switch (message?.type) {
        case 'ping':
            send(ws, { type: 'pong', at: new Date().toISOString() });
            return;
        case 'list_sessions':
            send(ws, { type: 'sessions', sessions });
            return;
        case 'subscribe': {
            const id = message.session;
            const since = Number(message.since) || 0;
            send(ws, {
                type: 'subscribed',
                session: describe(id),
                transcript: transcriptOf(id).filter((e) => e.hub_sequence > since),
                logs: [],
                latest: transcriptOf(id).at(-1)?.hub_sequence ?? 0,
                transcript_epoch: epoch,
            });
            return;
        }
        case 'input': {
            const requestId = message.request_id ?? 'stub-request';
            send(ws, {
                type: 'accepted', action: 'input', session: message.session, request_id: requestId,
            });
            // The worker's admission, with the empty payload the real protocol
            // defines for this event.
            const envelope = append(message.session, 'input_admitted', {}, { request_id: requestId });
            broadcast({
                type: 'event', session: message.session,
                hub_seq: envelope.hub_sequence, envelope,
            });
            return;
        }
        default:
            return;
    }
}

server.listen(PORT, '127.0.0.1', () => {
    process.stdout.write(`stub hub on http://127.0.0.1:${PORT}\n`);
});
