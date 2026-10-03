/**
 * @file worker reconnect recovery through the real Hub and panel client.
 *
 * Both channels use real WebSockets. Only the worker is a scripted peer;
 * disconnect notifications are produced by the Hub, not injected into the
 * panel. This catches wiring gaps that isolated frontend message tests miss.
 */
import assert from 'node:assert/strict';
import { rmSync } from 'node:fs';
import { it } from 'node:test';
import { WebSocket } from 'ws';
import { createPanelClient } from '../web/src/lib/client.ts';
import { memoryStorage } from '../web/src/lib/token.ts';
import { createPanelStore } from '../web/src/state/store.ts';
import { startTestHub } from './helpers/hub.js';
import { connectWorker, until, workerEvent } from './helpers/worker.js';

it('recovers pending cancel, options, and history after only the worker reconnects', async (t) => {
    const ctx = await startTestHub();
    const workers = [];
    const panelSockets = [];
    const messages = [];
    let client;
    t.after(async () => {
        client?.stop();
        for (const socket of panelSockets) socket.terminate();
        for (const worker of workers) await worker.close();
        await ctx.hub.stop();
        rmSync(ctx.config.dataDir, { recursive: true, force: true });
    });

    const session = ctx.hub.registry.create('worker-reconnect');
    const workerId = 'same-worker';
    const runId = 'active-run';
    let sequence = 0;
    async function connect() {
        const worker = await connectWorker(
            `${ctx.wsBase}/agent/${session.id}/events?token=${session.token}`);
        workers.push(worker);
        return worker;
    }
    function emit(worker, event, data) {
        sequence += 1;
        worker.send(workerEvent({ session: session.id, worker: workerId,
            sequence, runId, event, data }));
    }
    const status = { active: true, capabilities: ['session-history'] };
    const first = await connect();
    emit(first, 'status', status);
    await until(() => session.workerCapabilities?.names.includes('session-history'));

    // Observe the browser client's transport without replacing its handlers.
    class PanelWebSocket extends WebSocket {
        constructor(url) {
            super(url);
            panelSockets.push(this);
            this.on('message', (data) => messages.push(JSON.parse(data.toString('utf8'))));
        }
    }
    const store = createPanelStore();
    client = createPanelClient({
        store,
        location: new URL(`${ctx.base}/?session=${session.id}`),
        storage: memoryStorage(),
        fetchImpl: (url, options) => {
            const { pathname, search } = new URL(url);
            return fetch(`${ctx.base}${pathname}${search}`, options);
        },
        WebSocketImpl: PanelWebSocket,
    });
    await client.start();
    await until(() => messages.some((message) => message.type === 'welcome'),
        { label: 'real panel welcome' });
    assert.equal(store.getState().selected, session.id);
    await until(() => messages.some((message) => message.type === 'subscribed'),
        { label: 'real panel subscription' });
    const view = () => store.getState().views.get(session.id);
    const optionsRequest = (message) => message.type === 'signal'
        && message.data.operation === 'options';
    const historyRequest = (message) => message.type === 'payload'
        && message.data.operation === 'history';
    await first.waitFor(optionsRequest);
    const initialHistory = await first.waitFor(historyRequest);
    await until(() => view()?.historyLoading === true);
    assert.equal(view().modelCatalog, null);

    assert.equal(client.sendSignal(session.id, 'cancel', runId), true);
    await first.waitFor((message) => message.type === 'signal'
        && message.data.operation === 'cancel');
    assert.equal(view().cancelPending, true);
    assert.equal(client.sendSignal(session.id, 'cancel', runId), false);

    // Leave all three requests unanswered, then close only the worker socket.
    const panelSocket = panelSockets[0];
    await first.close();
    await until(() => messages.some((message) => message.type === 'session'
        && message.session.session_id === session.id && !message.session.connected),
    { label: 'disconnected session snapshot' });
    const disconnects = () => messages.filter((message) => message.type === 'connection'
        && message.session === session.id && !message.connected);
    assert.equal(disconnects().length, 1);
    assert.equal(disconnects()[0].identity.state, 'stale');
    assert.equal(store.getState().sessions.get(session.id).connected, false);
    assert.equal(view().cancelPending, false);
    assert.equal(view().historyLoading, false);
    assert.equal(panelSocket.readyState, WebSocket.OPEN);
    assert.equal(store.getState().connection.state, 'open');

    const second = await connect();
    emit(second, 'status', status);
    await second.waitFor(optionsRequest, { label: 'options retry on worker reconnect' });
    const refreshedHistory = await second.waitFor(historyRequest,
        { label: 'history retry on worker reconnect' });
    assert.notEqual(refreshedHistory.data.request_id, initialHistory.data.request_id);
    await until(() => view()?.historyLoading === true && view()?.runActive === true);
    assert.equal(client.sendSignal(session.id, 'cancel', runId), true);
    await second.waitFor((message) => message.type === 'signal'
        && message.data.operation === 'cancel', { label: 'cancel retry on worker reconnect' });

    emit(second, 'options', { model: {
        available: [{ name: 'model', options: ['test-model'] }],
        current: { model: 'test-model' },
    } });
    emit(second, 'history', {
        request_id: refreshedHistory.data.request_id, revision: 1,
        start: 0, step: 0, next: 1, next_step: 0, total: 1,
        turns: [{ index: 0, user: [{ type: 'text', raw: 'recovered history', modality: 'text' }],
            steps: [], omitted_steps: 0 }],
    });
    await until(() => view()?.modelCatalog?.worker_id === workerId
        && view()?.historyLoading === false && view()?.history.length === 1,
    { label: 'options and history recovery' });
    assert.equal(view().history[0].user[0].raw, 'recovered history');

    // An overlapping replacement must not announce a worker disconnect or
    // clear the cancellation still waiting on this same worker's active run.
    const previousConnection = session.connection;
    const replacement = await connect();
    emit(replacement, 'status', status);
    assert.equal((await second.waitForClose()).code, 4001);
    await until(() => previousConnection.closedAt !== null
        && messages.filter((message) => message.type === 'connection'
            && message.session === session.id && message.connected).length === 2,
    { label: 'replacement connected without a false disconnect' });
    // The pong is a barrier for all earlier frames on this panel socket,
    // including any erroneous disconnect triggered by the old socket closing.
    assert.equal(client.ping(), true);
    await until(() => messages.some((message) => message.type === 'pong'),
        { label: 'panel round trip after replacement' });
    assert.equal(disconnects().length, 1);
    assert.equal(store.getState().sessions.get(session.id).connected, true);
    assert.equal(view().cancelPending, true);
    assert.equal(panelSockets.length, 1);
    assert.equal(panelSocket.readyState, WebSocket.OPEN);
    assert.equal(messages.filter((message) => message.type === 'welcome').length, 1);
});
