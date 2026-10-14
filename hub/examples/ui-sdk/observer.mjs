/** Example-owned read-only transport/projection. NOT part of the SDK or a production client. */
import { PANEL_PROTOCOL, PANEL_VERSION, PANEL_SOCKET_PATH } from '@hazer-bjtu/simplex-hub/ui';

/** Validate only fields this example consumes; never treat imported types as validation. */
const record = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const count = value => Number.isSafeInteger(value) && value >= 0;
const sessionList = value => Array.isArray(value) && value.every(item => record(item)
    && typeof item.session_id === 'string' && (item.kind === undefined || ['ordinary', 'headless'].includes(item.kind)));

/**
 * Discover public metadata and the authenticated session list at an explicit origin.
 * The token stays in memory; never log request URLs or persist credentials.
 */
export async function discover(base, token, fetcher = fetch) {
    const url = new URL(base);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password
        || url.search || url.hash || url.pathname !== '/') throw new Error('Use an explicit HTTP(S) Hub origin');
    const metaReply = await fetcher(new URL('/api/meta', url));
    if (!metaReply.ok) throw new Error(`Metadata HTTP ${metaReply.status}`);
    const meta = await metaReply.json();
    if (!record(meta) || !record(meta.protocol) || meta.protocol.name !== PANEL_PROTOCOL.name
        || meta.protocol.version !== PANEL_VERSION || !Array.isArray(meta.capabilities)
        || !meta.capabilities.every(value => typeof value === 'string')) throw new Error('Unsupported Hub metadata');
    const reply = await fetcher(new URL('/api/sessions', url), { headers: token ? { Authorization: `Bearer ${token}` } : {} });
    if (!reply.ok) throw new Error(`Session discovery HTTP ${reply.status}`);
    const body = await reply.json();
    if (!record(body) || !sessionList(body.sessions)) throw new Error('Invalid session discovery reply');
    return body.sessions;
}

/**
 * Observe one ordinary session. Each invocation starts a fresh replay from zero;
 * reconnect is explicit. No input, decision, lifecycle action or history query is sent.
 * onView receives a bounded list of text notices, never authoritative worker state.
 */
export function observe(base, token, session, onView, WebSocketImpl = WebSocket) {
    const endpoint = new URL(PANEL_SOCKET_PATH, base);
    endpoint.protocol = endpoint.protocol === 'https:' ? 'wss:' : 'ws:';
    if (token) endpoint.searchParams.set('token', token);
    const socket = new WebSocketImpl(endpoint);
    const request = `example-${crypto.randomUUID()}`;
    let cursor = 0;
    let epoch;
    let welcomed = false;
    let pending = true;
    let first = true;
    let lines = [];
    let incomplete = false;
    let terminal = false;
    const view = status => onView({ status, lines: [...lines], incomplete, cursor, epoch });
    const stop = detail => { terminal = true; view(detail); socket.close(); };
    const append = events => {
        for (const event of events) {
            if (!record(event) || event.session_id !== session || typeof event.event !== 'string'
                || !count(event.hub_sequence) || event.hub_sequence <= cursor) throw new Error('Invalid replay order');
            if (event.hub_sequence !== cursor + 1) incomplete = true;
            cursor = event.hub_sequence;
            lines.push(`${cursor} · ${event.event}`);
            if (lines.length > 200) { lines.shift(); incomplete = true; }
        }
    };
    socket.addEventListener('message', event => {
        if (terminal) return;
        try {
            if (typeof event.data !== 'string') throw new Error('Expected a text frame');
            const value = JSON.parse(event.data);
            if (!record(value) || typeof value.type !== 'string' || (value.v !== undefined && value.v !== PANEL_VERSION)) throw new Error('Invalid panel envelope');
            if (value.type === 'welcome') {
                if (welcomed || !record(value.hub) || !record(value.hub.protocol)
                    || value.hub.protocol.name !== PANEL_PROTOCOL.name || value.hub.protocol.version !== PANEL_VERSION
                    || !Array.isArray(value.hub.capabilities) || !sessionList(value.sessions)) throw new Error('Invalid welcome');
                if (!['transcript-pages', 'transcript-epoch'].every(c => value.hub.capabilities.includes(c))) return stop('Unsupported replay capabilities');
                if (typeof value.hub.transcript_epoch !== 'string') throw new Error('Missing transcript epoch');
                const selected = value.sessions.find(item => item.session_id === session);
                if (!selected || selected.kind === 'headless') return stop('Select an ordinary session');
                welcomed = true;
                epoch = value.hub.transcript_epoch;
                socket.send(JSON.stringify({ v: PANEL_VERSION, type: 'subscribe', session, since: 0,
                    paged: true, replace: true, request_id: request }));
                view('Replaying');
            } else if (value.type === 'subscribed') {
                if (!welcomed || value.request_id !== request) return; // Stale/superseded reply.
                if (!pending || !record(value.session) || value.session.session_id !== session
                    || value.transcript_epoch !== epoch || !Array.isArray(value.transcript)
                    || !count(value.latest) || typeof value.replay_more !== 'boolean') throw new Error('Invalid replay correlation');
                if (first) {
                    if (value.replay_reset !== true) throw new Error('Missing replacement boundary');
                    lines = []; cursor = 0; first = false;
                } else if (value.replay_reset) throw new Error('Unexpected replay reset');
                append(value.transcript);
                if (value.latest < cursor) throw new Error('Invalid replay cursor');
                if (value.latest > cursor) incomplete = true;
                cursor = value.latest;
                pending = value.replay_more;
                view(pending ? 'Replaying' : 'Live');
            } else if (value.type === 'event' && value.session === session) {
                if (pending || !welcomed) throw new Error('Live event before replay completion');
                if (!record(value.envelope) || value.hub_seq !== value.envelope.hub_sequence) throw new Error('Invalid live cursor');
                // Other panels can issue worker control queries. Their replies
                // are forwarded live without consuming a Hub transcript slot.
                if (['history', 'answer', 'answer_error'].includes(value.envelope.event)) return;
                append([value.envelope]); view('Live');
            } else if (value.type === 'connection' && value.session === session && value.connected === false) {
                view('Worker disconnected; transcript is historical');
            } else if (value.type === 'session_removed' && value.session === session) {
                stop('Session removed');
            } else if (value.type === 'error') {
                stop('Hub rejected the subscription; reconnect explicitly');
            }
        } catch {
            stop('Invalid or incomplete stream; reconnect explicitly');
        }
    });
    socket.addEventListener('error', () => stop('Connection failed; verify address and authentication'));
    socket.addEventListener('close', () => { if (!terminal) view('Disconnected; reconnect explicitly'); });
    return () => { terminal = true; socket.close(); };
}
