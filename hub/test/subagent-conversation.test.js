/** Correlated two-cursor history assembly and atomic publication. */
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { it } from 'node:test';
import { Session } from '../src/state/registry.ts';
import { ConversationProjection } from '../src/subagents/conversation.ts';
import { until } from './helpers/worker.js';
const part = raw => ({ type: 'text', modality: 'text', raw });
async function projection(t) {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-primary-history-'));
    const session = new Session({ id: 'history-projection' }); session.kind = 'headless';
    const sent = [];
    const connection = { isOpen: true, sendPayload: payload => { sent.push(payload.data); return { ok: true }; } };
    session.connection = connection; session.noteIdentity('worker');
    session.workerCapabilities = { workerId: 'worker', names: ['session-history'] };
    const view = new ConversationProjection(session, join(directory, 'conversation.json'), 4096);
    view.connectionChanged(connection);
    t.after(() => { view.stop(); rmSync(directory, { recursive: true, force: true }); });
    await until(() => sent.length === 1);
    function page(fields = {}, envelope = {}) {
        const request = sent.at(-1);
        view.event({ event: 'history', worker_id: 'worker', request_id: request.request_id,
            data: { request_id: request.request_id, revision: 1, start: request.start, step: request.step,
                total: 1, next: 1, next_step: 0, turns: [], ...fields }, ...envelope }, connection);
    }
    return { view, sent, page, session, connection };
}
it('assembles continuation steps and strips tools/reasoning while honoring truncation', async t => {
    const { view, sent, page } = await projection(t);
    page({ next: 0, next_step: 1, turns: [{ index: 0, user: [part('input')],
        steps: [{ index: 0, content: [{ ...part('answer'), truncated: true }], reasoning: part('PRIVATE'), tool_calls: 5 }], omitted_steps: 1 }] });
    assert.equal(sent.length, 2); assert.equal(sent[1].step, 1);
    page({ turns: [{ index: 0, user: [part('input')], steps: [{ index: 1, content: [part('continued')] }] }] });
    assert.equal(view.value.stale, false); assert.equal(view.value.turns[0].steps.length, 2);
    assert.equal(view.value.truncated, true);
    assert.doesNotMatch(JSON.stringify(view.value), /PRIVATE|reasoning|tool_calls/);
});
it('ignores wrong correlation and preserves old state after revision/cursor violations', async t => {
    const { view, sent, page } = await projection(t);
    page({ turns: [{ index: 0, user: [], steps: [] }] }, { worker_id: 'other' });
    page({ request_id: 'unrelated-history', turns: [{ index: 0, user: [], steps: [] }] });
    assert.equal(view.value.turns.length, 0);
    page({ next: 0, next_step: 1, turns: [{ index: 0, user: [], steps: [{ index: 0, content: [part('partial')] }] }] });
    assert.equal(sent.length, 2);
    page({ revision: 2, turns: [{ index: 0, user: [], steps: [{ index: 1, content: [part('changed')] }] }] });
    assert.equal(view.value.turns.length, 0); assert.equal(view.value.incomplete, true);
    await until(() => sent.length === 3);
    page({ total: 3, next: 3, turns: [{ index: 0, user: [], steps: [] }] });
    assert.equal(view.value.turns.length, 0);
});
it('rejects live changes during refresh and cancels queries at the lifetime boundary', async t => {
    const { view, sent, page, session, connection } = await projection(t);
    view.event({ event: 'input_committed', worker_id: 'worker', request_id: 'unknown', sequence: 1 }, connection);
    page({ turns: [{ index: 0, user: [], steps: [] }] });
    assert.equal(view.value.turns.length, 0);
    view.stop(); session.closing = true;
    const before = sent.length;
    view.event({ event: 'ready', worker_id: 'worker', sequence: 2 }, connection);
    await new Promise(resolve => setTimeout(resolve, 40));
    assert.equal(sent.length, before);
});

it('correlates history through data.request_id while the envelope describes an active run', async t => {
    const { view, page } = await projection(t);
    page({ turns: [{ index: 0, user: [part('input')], steps: [{ index: 0, content: [part('answer')] }] }] },
        { request_id: 'active-model-request', run_id: 'active-model-run' });
    assert.equal(view.value.stale, false);
    assert.equal(view.value.turns[0].steps[0].content[0].raw, 'answer');
});
