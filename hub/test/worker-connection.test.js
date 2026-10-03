/**
 * @file the worker-facing event connection, exercised against a stand-in
 * worker over a real WebSocket.
 */
import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import { buildPayload, buildSignal } from '../src/protocol/messages.ts';
import { IDENTITY } from '../src/state/registry.ts';
import { connectWorker, upgradeStatus, until, workerEvent } from './helpers/worker.js';
import { startTestHub } from './helpers/hub.js';

describe('worker event connection', () => {
    let ctx;
    const opened = [];
    const changes = [];
    let counter = 0;

    before(async () => {
        ctx = await startTestHub({}, {
            onConnectionChange: (session, connection) => {
                changes.push({ id: session.id, connection, snapshot: session.describe() });
            },
        });
    });

    after(async () => {
        for (const worker of opened) await worker.close();
        await ctx.hub.stop();
    });

    /** Open a session plus a connected stand-in worker. */
    async function pair(label) {
        counter += 1;
        const id = `${label}-${counter}`;
        const session = ctx.hub.registry.create(id);
        const worker = await connectWorker(`${ctx.wsBase}/agent/${id}/events?token=${session.token}`);
        opened.push(worker);
        return { id, session, worker };
    }

    it('asks for a status snapshot as soon as the upgrade completes', async () => {
        const { worker } = await pair('status');
        const signal = await worker.waitFor(
            (message) => message.type === 'signal',
            { label: 'status signal' });
        assert.deepEqual(signal.data, { operation: 'status' });
    });

    it('records identity, sequence, and run correlation', async () => {
        const { session, worker } = await pair('identity');
        worker.send(workerEvent({
            session: session.id,
            worker: 'worker-a',
            sequence: 1,
            event: 'input_admitted',
            requestId: 'req-1',
            runId: 'run-1',
        }));
        await until(() => session.identity.state === IDENTITY.live, { label: 'live identity' });
        assert.equal(session.identity.workerId, 'worker-a');
        assert.equal(session.stats.events, 1);
        assert.equal(session.lastRunId, 'run-1');
        assert.equal(session.lastEvent.request_id, 'req-1');
        assert.equal(session.describe().stats.events, 1);
    });

    it('counts sequence gaps and duplicates', async () => {
        const { session, worker } = await pair('sequence');
        worker.send(workerEvent({ session: session.id, sequence: 1, event: 'status' }));
        worker.send(workerEvent({ session: session.id, sequence: 4, event: 'status' }));
        worker.send(workerEvent({ session: session.id, sequence: 4, event: 'status' }));
        await until(() => session.stats.events === 3, { label: 'three events' });
        assert.equal(session.stats.gaps, 2);
        assert.equal(session.stats.duplicates, 1);
        assert.equal(session.connection.lastSequence, 4n);
    });

    it('notices a new worker incarnation for the same session', async () => {
        const { session, worker } = await pair('incarnation');
        worker.send(workerEvent({ session: session.id, worker: 'first', sequence: 1 }));
        await until(() => session.identity.workerId === 'first');
        worker.send(workerEvent({ session: session.id, worker: 'second', sequence: 1 }));
        await until(() => session.identity.workerId === 'second', { label: 'new identity' });
        assert.equal(session.stats.incarnations, 1);
    });

    it('keeps an unknown event and its payload intact', async () => {
        const { session, worker } = await pair('unknown');
        worker.send(workerEvent({
            session: session.id,
            event: 'invented_by_a_future_worker',
            data: { any: ['shape', 1] },
        }));
        await until(() => session.lastEvent?.event === 'invented_by_a_future_worker');
        assert.equal(session.lastEvent.known, false);
        assert.deepEqual(session.lastEvent.data, { any: ['shape', 1] });
        assert.equal(session.stats.protocolErrors, 0);
    });

    it('refuses an event that claims another session', async () => {
        const { id, session, worker } = await pair('mismatch');
        worker.send(workerEvent({ session: 'some-other-session', sequence: 1 }));
        await until(() => session.stats.protocolErrors === 1, { label: 'protocol error' });
        assert.equal(session.stats.events, 0);
        assert.ok(session.connection.protocolErrors[0].message.includes(id) === false);
        assert.match(session.connection.protocolErrors[0].message, /does not match route session/);
    });

    it('refuses a binary message', async () => {
        const { session, worker } = await pair('binary');
        worker.sendBinary(Buffer.from('{"type":"event"}'));
        await until(() => session.stats.protocolErrors === 1, { label: 'protocol error' });
        assert.match(session.connection.protocolErrors[0].message, /binary/);
    });

    it('sends payloads and signals to the worker', async () => {
        const { session, worker } = await pair('send');
        const payload = buildPayload({
            requestId: 'req-send',
            content: [{ type: 'text', raw: 'hello worker', modality: 'text' }],
        });
        assert.deepEqual(session.connection.sendPayload(payload), { ok: true });
        const received = await worker.waitFor((message) => message.type === 'payload');
        assert.equal(received.data.request_id, 'req-send');
        assert.deepEqual(received.data.content, [{ type: 'text', raw: 'hello worker', modality: 'text' }]);

        assert.deepEqual(session.connection.sendSignal(buildSignal({ operation: 'status' })), { ok: true });
        await worker.waitFor((message) => message.type === 'signal' && message.data.operation === 'status');
    });

    it('refuses to send once the connection is closed', async () => {
        const { session, worker } = await pair('closed-send');
        const connection = session.connection;
        await worker.close();
        await until(() => !session.connected, { label: 'disconnected session' });
        const result = connection.sendPayload(buildPayload({
            requestId: 'req-late',
            content: [{ type: 'text', raw: 'x', modality: 'text' }],
        }));
        assert.equal(result.ok, false);
        assert.match(result.error, /not connected/);
    });

    it('marks a known identity stale when the connection drops', async () => {
        const { session, worker } = await pair('stale');
        worker.send(workerEvent({ session: session.id, worker: 'worker-stale', sequence: 1 }));
        await until(() => session.identity.state === IDENTITY.live);
        await worker.close();
        await until(() => session.identity.state === IDENTITY.stale, { label: 'stale identity' });
        assert.equal(session.identity.workerId, null);
        assert.equal(session.identity.lastWorkerId, 'worker-stale');
        assert.equal(session.connected, false);
    });

    for (const abrupt of [false, true]) {
        it(`notifies observers after ${abrupt ? 'abrupt' : 'graceful'} disconnection bookkeeping`, async () => {
            const { session, worker } = await pair('disconnect');
            worker.send(workerEvent({ session: session.id, worker: 'disconnect-worker' }));
            await until(() => session.identity.state === IDENTITY.live);
            session.trackRequest('pending-request', 'message');
            session.trackRequest('admitted-request', 'message');
            session.noteRequestAdmitted('admitted-request');

            if (abrupt) worker.ws.terminate();
            else await worker.close();
            await until(() => changes.some((change) => change.id === session.id
                && change.connection === null), { label: 'disconnect notification' });

            const notifications = changes.filter((change) => change.id === session.id);
            assert.equal(notifications.length, 2);
            assert.notEqual(notifications[0].connection, null);
            const { connection, snapshot } = notifications[1];
            assert.equal(connection, null);
            assert.equal(snapshot.connected, false);
            assert.equal(snapshot.identity.state, IDENTITY.stale);
            assert.equal(snapshot.identity.worker_id, 'disconnect-worker');
            assert.equal(snapshot.worker_capabilities, null);
            assert.deepEqual(snapshot.requests.map((entry) => [entry.request_id, entry.state]), [
                ['pending-request', 'unknown'],
                ['admitted-request', 'admitted'],
            ]);
        });
    }

    it('supersedes an existing connection instead of rejecting the reconnect', async () => {
        const { id, session, worker } = await pair('supersede');
        const first = session.connection;
        worker.send(workerEvent({ session: id, worker: 'same-worker', sequence: 1 }));
        await until(() => session.identity.state === IDENTITY.live);
        session.trackRequest('pending-on-replacement', 'message');
        const replacement = await connectWorker(`${ctx.wsBase}/agent/${id}/events?token=${session.token}`);
        opened.push(replacement);
        const closed = await worker.waitForClose();
        await until(() => first.closedAt !== null, { label: 'superseded close bookkeeping' });
        assert.equal(closed.code, 4001);
        assert.equal(session.connected, true);
        assert.notEqual(session.connection, first);
        assert.equal(session.identity.state, IDENTITY.live);
        assert.equal(session.requests.get('pending-on-replacement').state, 'sent');
        assert.equal(changes.filter((change) => change.id === id).length, 2);
        assert.equal(changes.some((change) => change.id === id
            && change.connection === null), false);
        await replacement.waitFor((message) => message.type === 'signal',
            { label: 'status for the replacement connection' });
        replacement.send(workerEvent({ session: id, worker: 'worker-new', sequence: 1 }));
        await until(() => session.identity.workerId === 'worker-new');
    });

    it('rejects an unknown session, an invalid session id, and a bad token', async () => {
        assert.equal(
            await upgradeStatus(`${ctx.wsBase}/agent/nobody-here/events?token=x`), 404);
        assert.equal(
            await upgradeStatus(`${ctx.wsBase}/agent/bad%2Fid/events?token=x`), 404);
        const session = ctx.hub.registry.create('token-check');
        assert.equal(
            await upgradeStatus(`${ctx.wsBase}/agent/token-check/events?token=wrong`), 401);
        assert.equal(
            await upgradeStatus(`${ctx.wsBase}/agent/token-check/events`), 401);
        const worker = await connectWorker(
            `${ctx.wsBase}/agent/token-check/events?token=${session.token}`);
        opened.push(worker);
        assert.equal(session.connected, true);
    });

    it('closes a connection that keeps sending protocol errors', async () => {
        const { session, worker } = await pair('flood');
        for (let index = 0; index < 25; index += 1) {
            worker.sendBinary(Buffer.from('garbage'));
        }
        const closed = await worker.waitForClose({ timeout: 3000 });
        assert.equal(closed.code, 1008);
        assert.ok(session.stats.protocolErrors >= 20);
    });
});
