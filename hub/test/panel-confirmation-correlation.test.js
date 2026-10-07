import assert from 'node:assert/strict';
import { it } from 'node:test';
import { setupPanelHub } from './helpers/panel.js';

it('the real Panel API echoes submission identity on acceptance and both rejection paths', async t => {
    const ctx = await setupPanelHub(t);
    const target = ctx.hub.registry.create('approval-correlation');
    const { peer } = await ctx.panel();
    const request = { type: 'confirmation', session: target.id, confirmation_id: 'prompt',
        request_id: 'attempt-a', decision: 'approved', reason: 'operator decision' };
    target.prompts.set('prompt', { decide: () => ({ ok: false, error: 'not ready' }) });
    peer.send(request);
    const rejected = await peer.waitFor(message => message.error === 'confirmation_rejected');
    assert.deepEqual(rejected.request, request);
    assert.equal(rejected.session, target.id);
    assert.equal(rejected.confirmation_id, 'prompt');
    target.prompts.set('prompt', { decide: () => ({ ok: true }) });
    peer.send({ ...request, request_id: 'attempt-b' });
    const accepted = await peer.waitFor(message => message.type === 'accepted' && message.action === 'confirmation');
    assert.equal(accepted.request_id, 'attempt-b');
    assert.equal(accepted.confirmation_id, 'prompt');
    target.prompts.delete('prompt');
    peer.send({ ...request, request_id: 'attempt-c' });
    const retired = await peer.waitFor(message => message.error === 'unknown_confirmation');
    assert.equal(retired.request.request_id, 'attempt-c');
    assert.equal(retired.request.session, target.id);
});
