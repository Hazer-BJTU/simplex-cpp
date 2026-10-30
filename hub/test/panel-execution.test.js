/** Exercise the shared execution boundary without React render timing. */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createPanelClient } from '../web/src/lib/client.ts';
import { memoryStorage } from '../web/src/lib/token.ts';
import { createPanelStore } from '../web/src/state/store.ts';

async function setup(t) {
    const store = createPanelStore();
    const sessions = ['demo', 'other'].map(session_id => ({
        session_id, created_at: '', spec: {}, connected: true,
        identity: { state: 'live', worker_id: 'worker', since: null },
        worker_capabilities: ['context-compact'],
        stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 1 },
        last_run_id: '', last_event_at: null, last_event: null,
        confirmations: [], process: null, requests: [],
    }));
    const hub = { name: 'simplex-hub', version: 'test',
        protocol: { name: 'simplex-hub-panel', version: 1 }, worker_protocol: '1',
        capabilities: ['context-compact'], transcript_epoch: 'epoch' };
    let socket;
    class Socket {
        readyState = 0;
        sent = [];
        fail = false;
        constructor() { socket = this; }
        send(text) {
            if (this.fail) throw new Error('lost transport');
            this.sent.push(JSON.parse(text));
        }
        close() { this.readyState = 3; this.onclose?.(); }
        receive(message) { this.onmessage?.({ data: JSON.stringify({ v: 1, ...message }) }); }
    }
    const client = createPanelClient({ store, WebSocketImpl: Socket,
        location: new URL('http://localhost/?session=demo'), storage: memoryStorage(),
        fetchImpl: async path => new Response(JSON.stringify(path.endsWith('/meta') ? hub : { sessions })),
    });
    t.after(() => client.stop());
    await client.start();
    socket.readyState = 1;
    socket.onopen();
    socket.receive({ type: 'welcome', hub, sessions, subscriptions: [] });
    let sequence = 0;
    const emit = (event, request, data = {}, worker = 'worker') => socket.receive({
        type: 'event', session: 'demo', envelope: {
            type: 'event', event, session_id: 'demo', worker_id: worker,
            sequence: ++sequence, hub_sequence: sequence, run_id: 'run', request_id: request,
            data,
        },
    });
    const inputs = () => socket.sent.filter(frame => frame.type === 'input');
    return { store, client, socket, sessions, emit, inputs };
}

test('all execution paths snapshot current options and reserve admission synchronously', async t => {
    const { store, client, socket, emit, inputs } = await setup(t);
    store.getState().setConfirmMode('demo', 'approve');
    assert.equal(client.sendInput('demo', [{ type: 'text', raw: 'task' }]), true);
    const first = inputs().at(-1);
    assert.deepEqual(first.options, { confirmation: { mode: 'approve' } });
    assert.equal(client.sendInput('demo', [], 'continue'), false);
    assert.equal(client.sendInput('demo', [], 'compact'), false);
    // Receipt/admitted request tracking can precede the actual admission event.
    socket.receive({ type: 'request', session: 'demo', request: {
        request_id: first.request_id, operation: 'message', state: 'admitted',
        sent_at: '', settled_at: '', detail: '',
    } });
    assert.equal(client.sendInput('demo', [], 'continue'), false);
    emit('input_admitted', first.request_id, { operation: 'message' });
    assert.equal(store.getState().pendingExecutions.size, 0);
    assert.equal(client.sendInput('demo', [], 'continue'), false, 'active run still guards admission');
    emit('run_finished', first.request_id, { status: 'failed', failure: {
        stage: 'model_request', can_continue: true, error: 'fixture',
    } });
    store.getState().setConfirmMode('demo', 'ask');
    emit('options', undefined, { model: {
        available: [{ name: 'model', options: ['first', 'second'] }], current: { model: 'first' },
    } });
    store.getState().setModelOption('demo', 'model', 'second');
    assert.equal(client.sendInput('demo', [], 'continue'), true);
    const retry = inputs().at(-1);
    assert.deepEqual(retry.options, { confirmation: { mode: 'ask' }, model: { model: 'second' } });
    assert.equal(Object.hasOwn(retry, 'content'), false);
    emit('input_rejected', retry.request_id, { request_id: 'unrelated', message: 'other request' });
    assert.equal(client.sendInput('demo', [], 'compact'), false);
    emit('input_rejected', retry.request_id, { request_id: retry.request_id, message: 'rejected' });
    assert.equal(client.sendInput('demo', [], 'compact'), true);
    assert.deepEqual(inputs().at(-1).options, retry.options);
    assert.equal(client.sendInput('other', [{ type: 'text', raw: 'independent' }]), true);
});

test('send failure and Hub rejection release only their own admission reservation', async t => {
    const { store, client, socket, inputs } = await setup(t);
    socket.fail = true;
    assert.equal(client.sendInput('demo', [{ type: 'text', raw: 'draft' }]), false);
    assert.equal(store.getState().pendingExecutions.size, 0);
    assert.equal(store.getState().failedInput.parts[0].raw, 'draft');
    socket.fail = false;
    assert.equal(client.sendInput('demo', [], 'continue'), true);
    const request = inputs().at(-1);
    socket.receive({ type: 'error', session: 'demo', error: 'rejected', message: 'not available',
        request: { ...request, request_id: 'other' } });
    assert.equal(store.getState().pendingExecutions.size, 1);
    socket.receive({ type: 'error', session: 'demo', error: 'rejected', message: 'not available', request });
    assert.equal(store.getState().pendingExecutions.size, 0);
    assert.equal(client.sendInput('demo', [], 'compact'), true);
});

for (const change of ['disconnect', 'replacement', 'panel-close']) {
    test(`${change} abandons channel-local admission without replaying input`, async t => {
        const { store, client, socket, sessions, inputs } = await setup(t);
        assert.equal(client.sendInput('demo', [], 'continue'), true);
        const original = inputs().at(-1);
        if (change === 'panel-close') {
            client.stop();
        } else if (change === 'disconnect') {
            socket.receive({ type: 'connection', session: 'demo', connected: false });
            assert.equal(client.sendInput('demo', [], 'continue'), false);
            socket.receive({ type: 'connection', session: 'demo', connected: true });
        } else {
            socket.receive({ type: 'session', session: { ...sessions[0],
                identity: { state: 'live', worker_id: 'replacement', since: null } } });
        }
        assert.equal(store.getState().pendingExecutions.size, 0);
        assert.equal(inputs().length, 1, 'never resend after losing the outcome');
        if (change !== 'panel-close') {
            assert.equal(client.sendInput('demo', [], 'compact'), true);
            // Late settlement from the abandoned request cannot unlock the new one.
            socket.receive({ type: 'request', session: 'demo', request: {
                request_id: original.request_id, operation: 'continue', state: 'rejected',
                sent_at: '', settled_at: '', detail: '',
            } });
            assert.equal(store.getState().pendingExecutions.size, 1);
        }
    });
}

test('a reentrant store listener cannot send before the first outbox is published', async t => {
    const { store, client, inputs } = await setup(t);
    let attempted = false;
    let nested;
    const unsubscribe = store.subscribe(state => {
        if (!attempted && state.pendingExecutions.has('demo')) {
            attempted = true;
            nested = client.sendInput('demo', [], 'compact');
        }
    });
    t.after(unsubscribe);
    assert.equal(client.sendInput('demo', [], 'continue'), true);
    assert.equal(nested, false);
    assert.equal(inputs().length, 1);
});

test('transcript replacement cannot unlock pending admission or strand it after a correlated event', async t => {
    const { store, client, socket, sessions, emit, inputs } = await setup(t);
    assert.equal(client.sendInput('demo', [], 'continue'), true);
    const request = inputs().at(-1);
    socket.receive({ type: 'snapshot', session: sessions[0], transcript: [] });
    assert.equal(client.sendInput('demo', [], 'compact'), false);
    emit('input_admitted', request.request_id, { operation: 'continue' });
    assert.equal(store.getState().pendingExecutions.size, 0);
    assert.equal(client.sendInput('demo', [], 'compact'), false, 'admitted run is active');
    emit('run_finished', request.request_id, { status: 'completed' });
    assert.equal(client.sendInput('demo', [], 'compact'), true);
});
