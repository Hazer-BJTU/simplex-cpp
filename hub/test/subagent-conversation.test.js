/** Correlated two-cursor history assembly and atomic publication. */
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, readFileSync, statSync, renameSync, symlinkSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { it } from 'node:test';
import { Session } from '../src/state/registry.ts';
import { ConversationProjection } from '../src/subagents/conversation.ts';
import { until } from './helpers/worker.js';
const part = raw => ({ type: 'text', modality: 'text', raw });
async function projection(t, maxBytes = 4096, onStorageFailure = () => {}) {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-primary-history-'));
    const session = new Session({ id: 'history-projection' }); session.kind = 'headless';
    const sent = [];
    const connection = { isOpen: true, sendPayload: payload => { sent.push(payload.data); return { ok: true }; } };
    session.connection = connection; session.noteIdentity('worker');
    session.workerCapabilities = { workerId: 'worker', names: ['session-history'] };
    const path = join(directory, 'conversation.json');
    const view = new ConversationProjection(session, path, maxBytes, onStorageFailure);
    view.connectionChanged(connection);
    t.after(() => { view.stop(); rmSync(directory, { recursive: true, force: true }); });
    await until(() => sent.length === 1);
    function page(fields = {}, envelope = {}) {
        const request = sent.at(-1);
        view.event({ event: 'history', worker_id: 'worker', request_id: request.request_id,
            data: { request_id: request.request_id, revision: 1, start: request.start, step: request.step,
                total: 1, next: 1, next_step: 0, turns: [], ...fields }, ...envelope }, connection);
    }
    return { view, sent, page, session, connection, directory, path };
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

it('retains the newest answer when history exceeds the byte budget, including reconnect and restore', async t => {
    const { view, sent, page, connection, session, path } = await projection(t);
    const total = 12;
    const latest = 'LATEST answer ' + 'x'.repeat(1600);
    // Reconciliation must not overwrite an already observed live result.
    view.value.turns = [{ index: total - 1, user: [part('question')],
        steps: [{ index: 0, content: [part(latest)] }] }];
    function complete() {
        while (view.value.stale) {
            const start = sent.at(-1).start;
            page({ total, next: start + 1, turns: [{ index: start, user: [part('question')],
                steps: [{ index: 0, content: [part(start === total - 1 ? latest : 'old '.repeat(450))] }] }] });
        }
        assert.equal(view.value.turns.at(-1).index, total - 1);
        assert.equal(view.value.turns.at(-1).steps.at(-1).content[0].raw, latest);
        assert.equal(view.value.truncated, true);
        assert.equal(view.value.incomplete, true);
        assert.ok(statSync(path).size <= 4096);
    }
    complete();
    assert.equal(sent[1].start, total - 1);
    view.connectionChanged(null);
    const before = sent.length;
    view.connectionChanged(connection);
    await until(() => sent.length > before);
    complete();
    const restored = new ConversationProjection(session, path, 4096);
    t.after(() => restored.stop());
    assert.equal(restored.value.turns.at(-1).steps.at(-1).content[0].raw, latest);
    assert.equal(restored.value.stale, true);
});

it('publishes a bounded newest tail rather than an old prefix at the 64-page limit', async t => {
    const { view, sent, page, path } = await projection(t, 128 * 1024);
    const total = 1000;
    while (view.value.stale) {
        const index = sent.at(-1).start;
        page({ total, next: index + 1, turns: [{ index, user: [part(`question ${index}`)],
            steps: [{ index: 0, content: [part(`answer ${index}`)] }] }] });
        assert.ok(sent.length <= 64);
    }
    assert.equal(sent.length, 64);
    assert.equal(view.value.turns.at(-1).index, 999);
    assert.equal(view.value.turns.at(-1).steps[0].content[0].raw, 'answer 999');
    assert.equal(view.value.turns.length, 63);
    assert.equal(view.value.incomplete, true);
    assert.equal(view.value.truncated, true);
    assert.ok(statSync(path).size <= 128 * 1024);
});

it('jumps to the final steps of a heavily fragmented turn instead of losing its latest answer', async t => {
    const { view, sent, page, path } = await projection(t, 128 * 1024);
    while (view.value.stale) {
        const step = sent.at(-1).step;
        const final = step === 99;
        page({ next: final ? 1 : 0, next_step: final ? 0 : step + 1,
            turns: [{ index: 0, user: [part('question')], omitted_steps: 99 - step,
                steps: [{ index: step, content: [part(`answer ${step}`)] }] }] });
        assert.ok(sent.length <= 64);
    }
    assert.equal(sent[1].step, 68);
    assert.equal(sent.length, 33);
    assert.equal(view.value.turns[0].steps.length, 32);
    assert.equal(view.value.turns[0].steps.at(-1).content[0].raw, 'answer 99');
    assert.equal(view.value.truncated, true);
    assert.equal(view.value.incomplete, true);
    assert.ok(statSync(path).size <= 128 * 1024);
});

it('keeps readable UTF-8 text from an oversized final answer within the file budget', async t => {
    const { view, page, path } = await projection(t);
    page({ turns: [{ index: 0, user: [part('问题'.repeat(1000))],
        steps: [{ index: 0, content: [part('最新答复'.repeat(2000))] }] }] });
    const answer = view.value.turns[0].steps[0].content[0].raw;
    assert.ok(answer.length > 0);
    assert.doesNotMatch(answer, /\uFFFD/);
    assert.equal(view.value.truncated, true);
    assert.ok(statSync(path).size <= 4096);
});

/** Deterministic write failure, including when CI runs as root. Existing bytes remain readable. */
function failStorage(t, directory) {
    const backup = `${directory}-durable`;
    renameSync(directory, backup);
    symlinkSync(backup, directory, 'dir');
    t.after(() => rmSync(backup, { recursive: true, force: true }));
}

it('contains scheduled send/storage failures, keeps durable data and bounds retries after stop', async t => {
    let reports = 0;
    const { view, sent, directory, path, connection } = await projection(t, 4096,
        () => { reports += 1; throw new Error('observer also failed'); });
    const durable = readFileSync(path, 'utf8');
    failStorage(t, directory);
    // The outstanding first query fails; all later queries fail at the scheduled boundary.
    connection.sendPayload = payload => {
        sent.push(payload.data);
        if (sent.length % 2 === 0) throw new Error('send failed');
        return { ok: false };
    };
    view.event({ event: 'history_error', data: { request_id: sent[0].request_id } }, connection);
    await until(() => sent.length === 3);
    await new Promise(resolve => setTimeout(resolve, 80));
    assert.equal(sent.length, 3);
    assert.ok(reports >= 3);
    assert.equal(view.storageFailed, true);
    assert.equal(view.value.stale, true);
    assert.equal(view.value.incomplete, true);
    assert.equal(readFileSync(path, 'utf8'), durable);
    view.stop();
    rmSync(directory);
    await new Promise(resolve => setTimeout(resolve, 80));
    assert.equal(existsSync(directory), false);
    assert.equal(sent.length, 3);
});

it('contains timeout storage failure and cancels subsequent work without recreating stopped data', async t => {
    let reports = 0;
    const { view, sent, directory, path } = await projection(t, 4096, () => { reports += 1; });
    const durable = readFileSync(path, 'utf8');
    failStorage(t, directory);
    await until(() => reports > 0, { timeout: 4000 });
    assert.equal(view.storageFailed, true);
    assert.equal(view.value.stale, true);
    assert.equal(view.value.incomplete, true);
    assert.equal(readFileSync(path, 'utf8'), durable);
    view.stop();
    rmSync(directory);
    const before = sent.length;
    await new Promise(resolve => setTimeout(resolve, 80));
    assert.equal(sent.length, before);
    assert.equal(existsSync(directory), false);
});
