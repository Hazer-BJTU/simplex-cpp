/**
 * @file per-panel outbound limits over real WebSocket connections.
 *
 * The slow-reader test pauses a client's TCP socket. Boundary tests inject
 * bufferedAmount values to make exact byte admission independent of OS socket
 * sizes, while still exercising the real panel route and message encoding.
 */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { WebSocket } from 'ws';
import { PANEL_MAX_BUFFERED_BYTES } from '../src/panel/api.ts';
import { PANEL_VERSION } from '../shared/protocol.ts';
import { setupPanelHub as setup } from './helpers/panel.js';
import { until, workerEvent } from './helpers/worker.js';

it('terminates a paused panel within its budget while other channels keep working', async (t) => {
    const ctx = await setup(t);
    const session = ctx.hub.registry.create('backpressure');
    const worker = await ctx.connect(`/agent/${session.id}/events?token=${session.token}`);
    worker.send(workerEvent({ session: session.id, worker: 'pressure-worker',
        sequence: 1, event: 'status', runId: 'pressure-run', data: { active: true } }));
    await until(() => session.stats.events === 1);

    const slow = await ctx.panel();
    const healthy = await ctx.panel();
    const initial = await ctx.subscribe(slow.peer, session.id);
    await ctx.subscribe(healthy.peer, session.id);
    const confirmation = await ctx.connect(`/agent/${session.id}/confirm?token=${session.token}`);
    confirmation.send({ type: 'confirmation_request', data: {
        session_id: session.id, worker_id: 'pressure-worker', run_id: 'pressure-run',
        confirmation_id: 'pressure-confirm', call: {
            type: 'serial_write', security: 'require_confirm', id: 'pressure-call',
            name: 'run_command', arguments: { command: 'true' },
        },
    } });
    await healthy.peer.waitFor((message) => message.type === 'confirmation' && message.open);

    let maximumBuffered = 0;
    let terminations = 0;
    const send = slow.server.send;
    t.mock.method(slow.server, 'send', function (...args) {
        const result = send.apply(this, args);
        maximumBuffered = Math.max(maximumBuffered, this.bufferedAmount);
        assert.ok(this.bufferedAmount <= PANEL_MAX_BUFFERED_BYTES,
            `slow panel backlog reached ${this.bufferedAmount} bytes`);
        return result;
    });
    const terminate = slow.server.terminate;
    t.mock.method(slow.server, 'terminate', function () {
        terminations += 1;
        return terminate.call(this);
    });
    slow.peer.ws._socket.pause();

    const padding = 'x'.repeat(256 * 1024);
    let sequence = 1;
    // Yield to the healthy reader every two frames. The finite upper bound
    // exceeds ordinary OS socket buffers without attempting memory exhaustion.
    for (let batch = 0; batch < 128 && terminations === 0; batch += 1) {
        const marker = `batch-${batch}`;
        for (let index = 0; index < 2; index += 1) {
            ctx.hub.panel.broadcast({ type: 'logs', session: session.id,
                lines: [padding, marker] });
        }
        await until(() => healthy.peer.messages.some((message) => message.type === 'logs'
            && message.lines.at(-1) === marker), { label: 'healthy log reader' });
        sequence += 1;
        worker.send(workerEvent({ session: session.id, worker: 'pressure-worker',
            sequence, event: 'model_response', runId: 'pressure-run',
            data: { content: [{ type: 'text', raw: marker, modality: 'text' }] } }));
        await until(() => healthy.peer.messages.some((message) => message.type === 'event'
            && message.envelope.sequence === sequence), { label: 'live worker event' });
    }
    assert.equal(terminations, 1, 'a non-reading panel must be disconnected');
    assert.ok(maximumBuffered > 0, 'the test must exercise actual queued output');
    assert.ok(maximumBuffered <= PANEL_MAX_BUFFERED_BYTES);
    await until(() => ctx.hub.panel.clientCount() === 1, { label: 'slow panel removed' });
    assert.equal(session.connected, true);
    assert.equal(session.stats.events, sequence);
    assert.equal(healthy.peer.ws.readyState, WebSocket.OPEN);

    healthy.peer.send({ v: PANEL_VERSION, type: 'confirmation', session: session.id,
        confirmation_id: 'pressure-confirm', decision: 'approved' });
    const decision = await confirmation.waitFor((message) => message.type === 'confirmation_response');
    assert.equal(decision.data.decision, 'approved');
    healthy.peer.send({ v: PANEL_VERSION, type: 'ping' });
    await healthy.peer.waitFor((message) => message.type === 'pong');
    assert.equal(terminations, 1, 'later broadcasts must not revisit the terminated panel');

    slow.peer.ws._socket.resume();
    assert.equal((await slow.peer.waitForClose()).code, 1006);
    const recovered = await ctx.panel();
    const replay = await ctx.subscribe(recovered.peer, session.id, initial.latest);
    assert.equal(replay.latest, sequence);
    assert.deepEqual(replay.transcript.map((event) => event.hub_sequence),
        Array.from({ length: sequence - initial.latest }, (_, index) => initial.latest + index + 1));
    assert.ok(replay.transcript.every((event) => event.event === 'model_response'));
    assert.equal(ctx.hub.panel.clientCount(), 2);
});

it('accounts for versioned UTF-8 JSON and all three frame-header sizes before sending', async (t) => {
    const ctx = await setup(t);
    for (const [line, expectedHeader] of [
        ['😀\u0001', 2],
        ['😀\u0001'.repeat(32), 4],
        ['😀\u0001'.repeat(7000), 10],
    ]) {
        const message = { type: 'logs', session: 'boundary', lines: [line] };
        const encoded = JSON.stringify({ v: PANEL_VERSION, ...message });
        const frameBytes = Buffer.byteLength(encoded, 'utf8') + expectedHeader;

        const accepted = await ctx.panel();
        Object.defineProperty(accepted.server, 'bufferedAmount', {
            configurable: true, get: () => PANEL_MAX_BUFFERED_BYTES - frameBytes,
        });
        ctx.hub.panel.broadcast(message);
        await accepted.peer.waitFor((received) => received.type === 'logs');
        assert.equal(accepted.peer.messages.find((received) => received.type === 'logs').lines[0], line);
        assert.equal(accepted.server.readyState, WebSocket.OPEN);
        await accepted.peer.close();
        await until(() => ctx.hub.panel.clientCount() === 0);

        const refused = await ctx.panel();
        Object.defineProperty(refused.server, 'bufferedAmount', {
            configurable: true, get: () => PANEL_MAX_BUFFERED_BYTES - frameBytes + 1,
        });
        const sends = t.mock.method(refused.server, 'send');
        ctx.hub.panel.broadcast(message);
        assert.equal(sends.mock.callCount(), 0, 'the next frame must not enter an over-budget queue');
        assert.equal(ctx.hub.panel.clientCount(), 0);
        assert.equal((await refused.peer.waitForClose()).code, 1006);
    }
});

it('rejects an oversized escaped JSON frame even with no queued output', async (t) => {
    const ctx = await setup(t);
    const panel = await ctx.panel();
    assert.equal(panel.server.bufferedAmount, 0);
    const sends = t.mock.method(panel.server, 'send');
    // Each input byte becomes six JSON bytes; raw string length is not a wire budget.
    const line = '\u0001'.repeat(Math.ceil(PANEL_MAX_BUFFERED_BYTES / 6));
    ctx.hub.panel.broadcast({ type: 'logs', session: 'oversized', lines: [line] });
    assert.equal(sends.mock.callCount(), 0);
    assert.equal(ctx.hub.panel.clientCount(), 0);
    assert.equal((await panel.peer.waitForClose()).code, 1006);

    const next = await ctx.panel();
    next.peer.send({ v: PANEL_VERSION, type: 'ping' });
    await next.peer.waitFor((message) => message.type === 'pong');
});
