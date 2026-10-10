/** Legacy subscription replies have no IDs, so delayed deltas must be fenced. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { createPanelClient } from '../web/src/lib/client.ts';
import { memoryStorage } from '../web/src/lib/token.ts';
import { createPanelStore } from '../web/src/state/store.ts';

function session(id) {
    return { session_id: id, created_at: '', spec: {}, connected: false,
        identity: { state: 'idle', worker_id: null, since: null },
        stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 0 },
        last_run_id: '', last_event_at: null, last_event: null,
        confirmations: [], process: null, requests: [] };
}

function event(sequence) {
    return { type: 'event', event: 'model_response', session_id: 'demo', worker_id: 'worker',
        hub_sequence: sequence, sequence, run_id: 'run', request_id: 'input', data: {} };
}

async function setup(t) {
    const store = createPanelStore();
    const sessions = [session('demo'), session('pressure')];
    const hub = { name: 'simplex-hub', version: 'legacy',
        protocol: { name: 'simplex-hub-panel', version: 1 }, worker_protocol: '1',
        capabilities: ['transcript-replay'], transcript_epoch: 'epoch' };
    let socket;
    class LegacySocket {
        readyState = 0;
        sent = [];
        constructor() { socket = this; }
        send(text) { this.sent.push(JSON.parse(text)); }
        close() { this.readyState = 3; this.onclose?.(); }
        receive(message) { this.onmessage?.({ data: JSON.stringify({ v: 1, ...message }) }); }
    }
    const client = createPanelClient({ store, WebSocketImpl: LegacySocket,
        location: new URL('http://localhost/?session=demo'), storage: memoryStorage(),
        fetchImpl: async path => new Response(JSON.stringify(path.endsWith('/meta') ? hub : { sessions })),
    });
    t.after(() => client.stop());
    await client.start();
    socket.readyState = 1;
    socket.onopen();
    socket.receive({ type: 'welcome', hub, sessions, subscriptions: [] });
    const reply = transcript => socket.receive({ type: 'subscribed', session: sessions[0],
        transcript, latest: transcript.at(-1)?.hub_sequence ?? 0, logs: [] });
    const cursors = () => socket.sent.filter(message => message.type === 'subscribe'
        && message.session === 'demo').map(message => message.since);
    return { store, client, reply, cursors, socket };
}

for (const queued of [false, true]) {
    it(`releases a rejected legacy subscription ${queued ? 'and sends the queued retry' : 'before a later retry'}`, async t => {
        const { store, client, reply, cursors, socket } = await setup(t);
        if (queued) client.subscribe('demo');
        assert.deepEqual(cursors(), [0]);
        socket.receive({ type: 'error', error: 'unknown_session', message: 'session not available',
            request: { type: 'subscribe', session: 'demo', since: 0 } });
        assert.equal(store.getState().notice.code, 'unknown_session');
        assert.equal(store.getState().connection.state, 'open');
        assert.equal(socket.readyState, 1);
        assert.deepEqual(cursors(), queued ? [0, 0] : [0], 'do not automatically retry rejected requests');
        if (!queued) client.subscribe('demo');
        assert.deepEqual(cursors(), [0, 0]);
        reply([event(1)]);
        assert.equal(store.getState().lastSeq('demo'), 1);
        client.subscribe('demo');
        assert.deepEqual(cursors(), [0, 0, 1], 'subsequent subscriptions must also remain unblocked');
    });
}

it('only a matching legacy subscription error releases its pending request', async t => {
    const { client, cursors, socket } = await setup(t);
    client.subscribe('demo');
    for (const request of [
        { type: 'subscribe', session: 'pressure', since: 0 },
        { type: 'unsubscribe', session: 'demo' },
        { type: 'signal', session: 'demo', operation: 'options' },
        { type: 'subscribe', session: 'demo', since: 10 },
        { type: 'subscribe', session: 'demo', since: 0, request_id: 'paged-request' },
    ]) {
        socket.receive({ type: 'error', error: 'unknown_session', request });
        assert.deepEqual(cursors(), [0]);
    }
    // Older error envelopes may identify the operation/session without echoing since.
    socket.receive({ type: 'error', error: 'unknown_session',
        request: { type: 'subscribe', session: 'demo' } });
    assert.deepEqual(cursors(), [0, 0]);
});

for (const cursor of [0, 4]) {
    for (const reselectFirst of [false, true]) {
        it(`ignores a pre-eviction since=${cursor} reply arriving ${reselectFirst ? 'after' : 'before'} reselect`, async t => {
            const { store, client, reply, cursors } = await setup(t);
            const prefix = Array.from({ length: 4 }, (_, index) => event(index + 1));
            if (cursor === 4) {
                reply(prefix);
                client.subscribe('demo');
            } else {
                // Initial zero-cursor replay is still outstanding.
                for (const envelope of prefix) store.getState().applyEvent({ session: 'demo', envelope });
            }
            assert.equal(cursors().at(-1), cursor);
            client.select('pressure');
            for (let index = 0; index < 9; ++index) {
                const id = `large-${index}`;
                store.getState().applyEvent({ session: id,
                    envelope: { ...event(1), session_id: id, data: { raw: 'x'.repeat(5 * 1024 * 1024) } } });
            }
            assert.equal(store.getState().view('demo').replayRequired, true);
            assert.equal(store.getState().lastSeq('demo'), 0);
            const generation = store.getState().view('demo').replayGeneration;
            if (reselectFirst) client.select('demo');
            const sentBeforeLateReply = cursors().length;
            // Also retain the already-covered late-live-frame boundary.
            store.getState().applyEvent({ session: 'demo', envelope: event(5) });
            reply(cursor === 4 ? [event(5)] : prefix);
            assert.equal(store.getState().view('demo').replayRequired, true);
            assert.deepEqual(store.getState().items('demo').map(item => item.envelope.hub_sequence), [5]);
            if (!reselectFirst) {
                assert.equal(cursors().length, sentBeforeLateReply, 'do not recover an inactive session');
                client.select('demo');
            }
            assert.equal(cursors().at(-1), 0);
            assert.equal(cursors().length, cursor === 4 ? 3 : 2);
            reply([...prefix, event(5)]);
            assert.equal(store.getState().view('demo').replayRequired, false);
            assert.equal(store.getState().view('demo').replayGeneration, generation);
            assert.deepEqual(store.getState().items('demo').map(item => item.envelope.hub_sequence), [1, 2, 3, 4, 5]);
            assert.equal(store.getState().lastSeq('demo'), 5);
            assert.equal(store.getState().connection.refusal, null);
        });
    }
}
