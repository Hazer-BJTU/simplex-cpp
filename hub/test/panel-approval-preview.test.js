import assert from 'node:assert/strict';
import { it } from 'node:test';
import { setupPanelHub } from './helpers/panel.js';
import { until, workerEvent } from './helpers/worker.js';
import { WebSocket } from 'ws';
import { PANEL_FRAME_BYTES } from '../src/panel/api.ts';
import { APPROVAL_SNAPSHOT_ARGUMENT_BYTES } from '../src/protocol/approval-snapshot.ts';
import { createPanelClient } from '../web/src/lib/client.ts';
import { memoryStorage } from '../web/src/lib/token.ts';
import { createPanelStore } from '../web/src/state/store.ts';

it('carries labelled argument previews through real confirmation, Panel WS and REST without changing authority', async t => {
    const ctx = await setupPanelHub(t);
    const target = ctx.hub.registry.create('approval-preview-wire');
    const events = await ctx.connect(`/agent/${target.id}/events?token=${target.token}`);
    events.send(workerEvent({ session: target.id, worker: 'worker-preview', sequence: 1,
        event: 'status', data: { active: false } }));
    await until(() => target.identity.state === 'live', { label: 'live identity' });
    const { peer } = await ctx.panel();
    const source = Object.fromEntries(Array.from({ length: 10 }, (_, index) => [`field_${index}`, 'x'.repeat(20000)]));
    source.path = '/workspace/file.txt';
    const call = { name: 'str_replace_edit', id: 'original-call', type: 'serial_write',
        security: 'require_confirm', arguments: source };
    const confirmation = await ctx.connect(`/agent/${target.id}/confirm?token=${target.token}`);
    confirmation.send({ type: 'confirmation_request', data: {
        worker_id: 'worker-preview', session_id: target.id, run_id: 'run-preview',
        confirmation_id: 'preview-wire', call,
    } });
    const message = await peer.waitFor(value => value.type === 'confirmation' && value.open);
    const preview = message.confirmation;
    assert.equal(preview.arguments_truncated, true);
    assert.ok(Buffer.byteLength(JSON.stringify(preview.call.arguments)) <= 64 * 1024);
    assert.ok(Buffer.byteLength(JSON.stringify(message)) < 70 * 1024);
    assert.deepEqual(Object.keys(preview.call.arguments), Object.keys(source));
    assert.equal(preview.call.arguments.path, source.path);
    for (let index = 0; index < 10; index += 1) {
        assert.equal(preview.call.arguments[`field_${index}`].display_truncated, true);
    }
    const response = await fetch(`${ctx.base}/api/sessions/${target.id}`);
    assert.equal(response.status, 200);
    const body = await response.json();
    assert.deepEqual(body.session.confirmations[0].call.arguments, preview.call.arguments);
    assert.deepEqual(target.prompts.get('preview-wire').call.arguments, source);
    peer.send({ type: 'confirmation', session: target.id, confirmation_id: 'preview-wire',
        request_id: 'operator-decision', decision: 'approved', reason: 'reviewed preview' });
    const decision = await confirmation.waitFor(value => value.type === 'confirmation_response');
    assert.equal(decision.data.decision, 'approved');
    assert.equal(decision.data.confirmation_id, 'preview-wire');
});

for (const counts of [[32], [64], [32, 32]]) {
    it(`initializes, pages and reconnects the real panel with ${counts.join('+')} large pending approvals`, async t => {
        const ctx = await setupPanelHub(t);
        const sessions = [];
        const workers = [];
        const confirmations = [];
        const command = 'x'.repeat(65500);
        for (const [index, count] of counts.entries()) {
            const session = ctx.hub.registry.create(`many-approvals-${index}`);
            sessions.push(session);
            const worker = await ctx.connect(`/agent/${session.id}/events?token=${session.token}`);
            workers.push(worker);
            worker.send(workerEvent({ session: session.id, worker: 'preview-worker', sequence: 1,
                event: 'status', data: { active: false, capabilities: ['session-history'] } }));
            await until(() => session.identity.state === 'live');
            for (let number = 0; number < count; number++) {
                const peer = await ctx.connect(`/agent/${session.id}/confirm?token=${session.token}`);
                confirmations.push(peer);
                peer.send({ type: 'confirmation_request', data: {
                    worker_id: 'preview-worker', session_id: session.id, run_id: 'preview-run',
                    confirmation_id: `approval-${number}`, call: { id: `call-${number}`, name: 'run_command',
                        type: 'serial_write', security: 'require_confirm', arguments: { command } },
                } });
                await until(() => session.prompts.size === number + 1);
            }
            assert.ok(Buffer.byteLength(JSON.stringify(session.describe())) > PANEL_FRAME_BYTES,
                'fixture must reproduce the oversized original session description');
        }
        const selected = sessions[0];
        // More than one replay page once its approval description is included.
        for (let sequence = 2; sequence <= 6; sequence++) workers[0].send(workerEvent({
            session: selected.id, worker: 'preview-worker', sequence, event: 'model_response',
            data: { content: [{ type: 'text', raw: 'answer '.repeat(600), modality: 'text' }] },
        }));
        await until(() => selected.stats.events === 6);

        const messages = [];
        const sockets = [];
        class PanelWebSocket extends WebSocket {
            constructor(url) {
                super(url);
                sockets.push(this);
                this.on('message', data => {
                    assert.ok(data.length <= PANEL_FRAME_BYTES);
                    messages.push(JSON.parse(data.toString('utf8')));
                });
            }
        }
        const store = createPanelStore();
        const client = createPanelClient({
            store, location: new URL(`${ctx.base}/?session=${selected.id}`),
            storage: memoryStorage(), WebSocketImpl: PanelWebSocket,
            fetchImpl: (url, options) => {
                const { pathname, search } = new URL(url);
                return fetch(`${ctx.base}${pathname}${search}`, options);
            },
        });
        t.after(() => { client.stop(); for (const socket of sockets) socket.terminate(); });
        const finalPages = () => messages.filter(message => message.type === 'subscribed' && !message.replay_more);
        const assertPrompts = () => {
            for (const [index, session] of sessions.entries()) {
                const prompts = store.getState().views.get(session.id).confirmations;
                assert.equal(prompts.size, counts[index]);
                for (const prompt of prompts.values()) {
                    assert.equal(prompt.verified, true);
                    assert.equal(prompt.state, 'awaiting-decision');
                    assert.equal(prompt.arguments_truncated, true);
                    assert.equal(prompt.arguments_bytes, Buffer.byteLength(JSON.stringify({ command })));
                    assert.equal(typeof prompt.call.arguments.command.preview, 'string');
                    assert.equal(prompt.call.arguments.command.bytes, command.length);
                    assert.ok(command.startsWith(prompt.call.arguments.command.preview));
                }
            }
        };
        await client.start();
        await until(() => finalPages().length === 1 && store.getState().lastSeq(selected.id) === 6,
            { timeout: 5000, label: 'fresh initialization and replay completion' });
        assertPrompts();
        assert.ok(messages.filter(message => message.type === 'subscribed').length > 1);
        sockets.at(-1).send(JSON.stringify({ type: 'list_sessions' }));
        await until(() => messages.some(message => message.type === 'sessions'));
        assert.equal(messages.find(message => message.type === 'sessions').sessions.length, sessions.length);

        sockets.at(-1).terminate();
        await until(() => messages.filter(message => message.type === 'welcome').length === 2
            && finalPages().length === 2, { timeout: 5000, label: 'reconnect restores approvals and subscription' });
        assertPrompts();
        assert.equal(store.getState().lastSeq(selected.id), 6);
        assert.equal(sockets.at(-1).readyState, WebSocket.OPEN);
        assert.equal(messages.some(message => message.error === 'display_snapshot_too_large'), false);
        for (const message of messages) {
            const descriptions = ['welcome', 'sessions'].includes(message.type) ? message.sessions
                : message.type === 'subscribed' ? [message.session] : [];
            const argumentBytes = descriptions.flatMap(session => session.confirmations)
                .reduce((sum, prompt) => sum + Buffer.byteLength(JSON.stringify(prompt.call.arguments)), 0);
            assert.ok(argumentBytes <= APPROVAL_SNAPSHOT_ARGUMENT_BYTES);
        }
        // Snapshot shortening never modifies the pending operation. REST and
        // individual prompt descriptions still expose the 64 KiB allowance.
        assert.equal(selected.prompts.get('approval-0').call.arguments.command, command);
        const rest = await (await fetch(`${ctx.base}/api/sessions/${selected.id}`)).json();
        assert.equal(rest.session.confirmations[0].arguments_truncated, undefined);
        assert.equal(rest.session.confirmations[0].call.arguments.command, command);
        assert.equal(client.sendConfirmation(selected.id, 'approval-0', 'approved', 'reviewed snapshot'), true);
        const decision = await confirmations[0].waitFor(message => message.type === 'confirmation_response');
        assert.equal(decision.data.decision, 'approved');
        await until(() => store.getState().views.get(selected.id).confirmations.size === counts[0] - 1);
    });
}
