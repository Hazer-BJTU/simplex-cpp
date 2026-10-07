import assert from 'node:assert/strict';
import { it } from 'node:test';
import { setTimeout as delay } from 'node:timers/promises';
import { createPanelStore } from '../web/src/state/store.ts';
import { createConfirmationDecisions, confirmationKey } from '../web/src/lib/confirmationDecisions.ts';

function setup(t, overrides = {}) {
    const panel = createPanelStore();
    const session = { session_id: 'demo', connected: true,
        identity: { worker_id: 'worker', state: 'live' }, confirmations: [] };
    panel.getState().upsertSession(session);
    panel.getState().setConnection({ state: 'open', attempt: 0, nextDelayMs: null });
    const sent = [];
    const controller = createConfirmationDecisions({ panel, timeoutMs: 15,
        send: (prompt, decision, reason, requestId) => { sent.push({ prompt, decision, reason, requestId }); return true; },
        snapshot: async () => ({ ...session, confirmations: [...panel.getState().views.get('demo').confirmations.values()] }),
        ...overrides });
    t.after(() => controller.stop());
    const prompt = { session_id: 'demo', worker_id: 'worker', run_id: 'run',
        confirmation_id: 'id', received_at: '2026-10-07T00:00:00Z', settled_at: null,
        deadline_at: null, call: { name: 'tool', id: 'call' } };
    function add(value = prompt) {
        panel.getState().applyConfirmation({ type: 'confirmation', session: value.session_id, open: true, confirmation: value });
    }
    function state(value = prompt) { return controller.states.getState().get(confirmationKey(value)); }
    add();
    return { panel, controller, prompt, session, sent, add, state };
}

it('suppresses duplicate/opposite decisions until authoritative settlement; another prompt stays usable', t => {
    const s = setup(t);
    assert.equal(s.controller.submit('demo', 'id', 'approved'), true);
    assert.equal(s.controller.submit('demo', 'id', 'approved'), false);
    assert.equal(s.controller.submit('demo', 'id', 'denied'), false);
    const other = { ...s.prompt, confirmation_id: 'other' }; s.add(other);
    assert.equal(s.controller.submit('demo', 'other', 'denied'), true);
    assert.equal(s.sent.length, 2);
    s.panel.getState().applyConfirmation({ type: 'confirmation', session: 'demo', open: false, confirmation: s.prompt });
    assert.equal(s.state(), undefined);
    assert.equal(s.state(other).phase, 'pending');
});

it('scopes rejection by submission ID and reconciles late/global responses without enabling an unsafe retry', async t => {
    const s = setup(t);
    s.controller.submit('demo', 'id', 'approved');
    s.controller.rejected({ type: 'error', error: 'confirmation_rejected', message: 'other prompt',
        request: { type: 'confirmation', session: 'demo', confirmation_id: 'other' } });
    assert.equal(s.state().phase, 'pending');
    s.controller.rejected({ type: 'error', error: 'confirmation_rejected', message: 'refused',
        request: { type: 'confirmation', session: 'demo', confirmation_id: 'id', request_id: s.sent[0].requestId } });
    assert.equal(s.state().retryable, true);
    assert.equal(s.state().error, 'refused');
    s.controller.submit('demo', 'id', 'denied');
    assert.notEqual(s.sent[1].requestId, s.sent[0].requestId);
    s.controller.rejected({ type: 'error', error: 'confirmation_rejected', message: 'late',
        request: { type: 'confirmation', session: 'demo', confirmation_id: 'id', request_id: s.sent[0].requestId } });
    assert.equal(s.state().phase, 'pending');
    await delay(0);
    assert.equal(s.state().retryable, false);
    assert.notEqual(s.state().error, 'late');
});

it('missing acknowledgement checks the Hub and never resends automatically', async t => {
    const s = setup(t);
    s.controller.submit('demo', 'id', 'approved');
    await delay(35);
    assert.equal(s.sent.length, 1);
    assert.equal(s.state().retryable, true);
    assert.match(s.state().error, /still lists this prompt as open/);
    assert.equal(s.controller.submit('demo', 'id', 'denied'), true);
});

it('missing settlement removes a no-longer-open prompt without synthesizing an approval', async t => {
    const s = setup(t, { snapshot: async () => ({ session_id: 'demo', identity: { worker_id: 'worker' }, confirmations: [] }) });
    s.controller.submit('demo', 'id', 'approved');
    await delay(35);
    assert.equal(s.panel.getState().confirmation('demo', 'id'), null);
    assert.equal(s.state(), undefined);
    assert.equal(s.sent.length, 1);
});

it('a failed reconciliation remains recoverable through explicit review; no conflicting retry is admitted', async t => {
    let succeeds = false;
    const s = setup(t, { snapshot: async () => {
        if (!succeeds) throw Error('offline');
        return { ...s.session, confirmations: [s.prompt] };
    } });
    s.controller.submit('demo', 'id', 'approved');
    await delay(35);
    assert.equal(s.state().retryable, false);
    assert.equal(s.controller.submit('demo', 'id', 'denied'), false);
    succeeds = true;
    s.controller.review(s.prompt);
    await delay(0);
    assert.equal(s.state().retryable, true);
});

for (const link of ['panel', 'worker']) {
    it(`recovers ${link} disconnect independently and rejects stale checks after worker replacement`, async t => {
        let resolve;
        const s = setup(t, { snapshot: () => new Promise(done => { resolve = done; }) });
        s.controller.submit('demo', 'id', 'approved');
        if (link === 'panel') s.panel.getState().setConnection({ state: 'closed', attempt: 0, nextDelayMs: null });
        else s.panel.getState().applyConnection({ type: 'connection', session: 'demo', connected: false, identity: s.session.identity });
        assert.equal(s.state().phase, 'failed');
        assert.equal(s.state().retryable, false);
        if (link === 'panel') s.panel.getState().setConnection({ state: 'open', attempt: 0, nextDelayMs: null });
        else s.panel.getState().applyConnection({ type: 'connection', session: 'demo', connected: true, identity: s.session.identity });
        assert.equal(s.state().phase, 'checking');
        const replacement = { ...s.prompt, worker_id: 'replacement', received_at: '2026-10-07T01:00:00Z' };
        s.add(replacement);
        s.panel.getState().upsertSession({ ...s.session, identity: { worker_id: 'replacement', state: 'live' } });
        resolve({ ...s.session, confirmations: [] });
        await delay(0);
        assert.equal(s.panel.getState().confirmation('demo', 'id'), replacement);
        assert.equal(s.state(), undefined);
        assert.equal(s.sent.length, 1);
    });
}

it('expiry and stopping the client fence all later submissions and async work', t => {
    const s = setup(t);
    s.add({ ...s.prompt, deadline_at: '2020-01-01T00:00:00Z' });
    assert.equal(s.controller.submit('demo', 'id', 'approved'), false);
    s.add();
    s.controller.submit('demo', 'id', 'approved');
    s.controller.stop();
    assert.equal(s.controller.states.getState().size, 0);
    assert.equal(s.controller.submit('demo', 'id', 'denied'), false);
});

it('authoritative reconnect snapshots replace reused prompt IDs and changed verification flags', t => {
    const s = setup(t);
    s.controller.submit('demo', 'id', 'approved');
    const changed = { ...s.prompt, verified: true, identity_state: 'live' };
    function welcome(prompt, worker) {
        s.panel.getState().applyWelcome({ type: 'welcome', sessions: [{ ...s.session,
            identity: { worker_id: worker, state: 'live' }, confirmations: [prompt] }],
            subscriptions: [], hub: { name: 'hub', version: 'test', capabilities: [], transcript_epoch: null } });
    }
    welcome(changed, 'worker');
    assert.equal(s.panel.getState().confirmation('demo', 'id').verified, true);
    const replacement = { ...changed, worker_id: 'next', received_at: '2026-10-07T01:00:00Z' };
    welcome(replacement, 'next');
    assert.equal(s.panel.getState().confirmation('demo', 'id'), replacement);
    assert.equal(s.state(), undefined);
    assert.equal(s.controller.submit('demo', 'id', 'denied'), true);
    assert.equal(s.sent.at(-1).prompt.worker_id, 'next');
});
