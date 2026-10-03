/** @file bounded replay recovery through the real Hub and bundled panel client. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { WebSocket } from 'ws';
import { PANEL_MAX_BUFFERED_BYTES, PANEL_REPLAY_PAGE_BYTES } from '../src/panel/api.ts';
import { PANEL_VERSION } from '../shared/protocol.ts';
import { createPanelClient } from '../web/src/lib/client.ts';
import { memoryStorage } from '../web/src/lib/token.ts';
import { createPanelStore } from '../web/src/state/store.ts';
import { setupPanelHub } from './helpers/panel.js';
import { until, workerEvent } from './helpers/worker.js';

for (const reconnect of [false, true]) {
    it(`recovers more than 4 MiB of replay in a ${reconnect ? 'reconnecting' : 'fresh'} panel`, async (t) => {
        let release;
        let held = false;
        const sentPages = [];
        const ctx = await setupPanelHub(t, (server) => {
            const send = server.send;
            t.mock.method(server, 'send', function (text, callback) {
                const message = JSON.parse(text);
                if (message.type === 'subscribed') {
                    sentPages.push(message);
                    assert.ok(Buffer.byteLength(text, 'utf8') + 10 <= PANEL_REPLAY_PAGE_BYTES);
                }
                if (message.type === 'subscribed' && message.replay_more && !held) {
                    held = true;
                    return send.call(this, text, (error) => {
                        release = (failure = error) => callback(failure);
                    });
                }
                return send.call(this, text, callback);
            });
        });
        const session = ctx.hub.registry.create('long-replay');
        const worker = await ctx.connect(`/agent/${session.id}/events?token=${session.token}`);
        const emit = (sequence, event, data) => worker.send(workerEvent({
            session: session.id, worker: 'replay-worker', sequence, event, data,
        }));
        emit(1, 'status', { active: false, capabilities: ['session-history'] });
        for (let sequence = 2; sequence <= 1001; sequence += 1) {
            emit(sequence, 'model_response', { content: [
                { type: 'text', raw: 'x'.repeat(2048), modality: 'text' },
            ] });
        }
        await until(() => session.stats.events === 1001, { timeout: 5000 });
        const retained = ctx.hub.transcripts.get(session.id);
        assert.ok(Buffer.byteLength(JSON.stringify(retained.toArray()), 'utf8')
            > PANEL_MAX_BUFFERED_BYTES, 'the full replay must exceed one outbound budget');

        const messages = [];
        const sockets = [];
        class PanelWebSocket extends WebSocket {
            constructor(url) {
                super(url);
                sockets.push(this);
                this.on('message', (data) => messages.push(JSON.parse(data.toString('utf8'))));
            }
        }
        const store = createPanelStore();
        const client = createPanelClient({
            store, location: new URL(`${ctx.base}/?session=${session.id}`),
            storage: memoryStorage(), WebSocketImpl: PanelWebSocket,
            fetchImpl: (url, options) => {
                const { pathname, search } = new URL(url);
                return fetch(`${ctx.base}${pathname}${search}`, options);
            },
        });
        t.after(() => {
            release?.(new Error('test cleanup'));
            client.stop();
            for (const socket of sockets) socket.terminate();
        });
        await client.start();
        await until(() => release && store.getState().lastSeq(session.id) > 0);
        const partialCursor = store.getState().lastSeq(session.id);
        assert.ok(partialCursor < 1001, 'a partial page must not claim the whole transcript');
        assert.equal(sentPages.length, 1, 'the next page must wait for the previous write');
        assert.equal(worker.messages.some((message) => message.type === 'payload'), false);
        assert.equal(worker.messages.some((message) => message.type === 'signal'
            && message.data.operation === 'options'), false);

        // This must be replayed before live delivery begins, not overtaken by
        // a live event that advances the cursor past the unfinished history.
        emit(1002, 'model_response', { content: [
            { type: 'text', raw: 'arrived during replay', modality: 'text' },
        ] });
        await until(() => session.stats.events === 1002);
        if (reconnect) ctx.panelServers[0].terminate();
        release();
        release = null;
        await until(() => store.getState().lastSeq(session.id) === 1002
            && worker.messages.some((message) => message.type === 'payload'
                && message.data.operation === 'history'), { timeout: 5000, label: 'complete replay recovery' });
        const replies = messages.filter((message) => message.type === 'subscribed');
        assert.ok(replies.length > 2);
        assert.equal(replies.at(-1).replay_more, false);
        assert.equal(replies.at(-1).latest, 1002);
        assert.deepEqual(replies.flatMap((message) => message.transcript.map((event) => event.hub_sequence)),
            Array.from({ length: 1002 }, (_, index) => index + 1));
        assert.equal(store.getState().views.get(session.id).gaps, 0);
        assert.equal(store.getState().views.get(session.id).items.filter((item) => item.kind === 'event').length,
            1002);
        assert.equal(sockets.length, reconnect ? 2 : 1);
        assert.equal(sockets.at(-1).readyState, WebSocket.OPEN);
        assert.equal(worker.messages.filter((message) => message.type === 'payload'
            && message.data.operation === 'history').length, 1);

        // Explicit refresh uses the same paced transport and replaces only
        // when its first page actually arrives, rather than requesting one
        // oversized status_snapshot frame.
        const completed = replies.filter((message) => message.replay_more === false).length;
        client.reloadTranscript(session.id);
        await until(() => messages.filter((message) => message.type === 'subscribed'
            && message.replay_more === false).length === completed + 1,
        { timeout: 5000, label: 'paged explicit refresh' });
        assert.equal(store.getState().lastSeq(session.id), 1002);
        assert.equal(store.getState().views.get(session.id).items.filter((item) => item.kind === 'event').length,
            1002);
        assert.equal(sockets.at(-1).readyState, WebSocket.OPEN);
    });
}

it('invalidates a pending replay on replacement or unsubscribe', async (t) => {
    const ctx = await setupPanelHub(t);
    const session = ctx.hub.registry.create('replay-generations');
    const transcript = ctx.hub.transcripts.get(session.id);
    transcript.append(workerEvent({ session: session.id }));
    const panel = await ctx.panel();
    const pending = new Map();
    const send = panel.server.send;
    t.mock.method(panel.server, 'send', function (text, callback) {
        const message = JSON.parse(text);
        if (message.type === 'subscribed' && message.replay_more) {
            return send.call(this, text, (error) => pending.set(message.request_id, () => callback(error)));
        }
        return send.call(this, text, callback);
    });
    t.after(() => { for (const release of pending.values()) release(); });
    const subscribe = (id) => panel.peer.send({ v: PANEL_VERSION, type: 'subscribe',
        session: session.id, paged: true, request_id: id });
    subscribe('old');
    await until(() => pending.has('old'));
    subscribe('replacement');
    await until(() => pending.has('replacement'));
    pending.get('old')();
    pending.delete('old');
    pending.get('replacement')();
    pending.delete('replacement');
    await panel.peer.waitFor((message) => message.type === 'subscribed'
        && message.request_id === 'replacement' && message.replay_more === false);
    assert.equal(panel.peer.messages.some((message) => message.type === 'subscribed'
        && message.request_id === 'old' && message.replay_more === false), false);
    subscribe('unsubscribed');
    await until(() => pending.has('unsubscribed'));
    panel.peer.send({ v: PANEL_VERSION, type: 'unsubscribe', session: session.id });
    panel.peer.send({ v: PANEL_VERSION, type: 'ping' });
    await panel.peer.waitFor((message) => message.type === 'pong');
    pending.get('unsubscribed')();
    pending.delete('unsubscribed');
    panel.peer.messages.length = 0;
    panel.peer.send({ v: PANEL_VERSION, type: 'ping' });
    await panel.peer.waitFor((message) => message.type === 'pong');
    assert.equal(panel.peer.messages.some((message) => message.type === 'subscribed'), false);
    assert.equal(panel.server.readyState, WebSocket.OPEN);
});
