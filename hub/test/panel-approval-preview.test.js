import assert from 'node:assert/strict';
import { it } from 'node:test';
import { setupPanelHub } from './helpers/panel.js';
import { until, workerEvent } from './helpers/worker.js';

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
