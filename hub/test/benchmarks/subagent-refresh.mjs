/**
 * Repeat the same delayed-history/live-event fixture against either checkout.
 * Run: node hub/test/benchmarks/subagent-refresh.mjs [repository-root]
 * Virtual time measures query counts, not machine-dependent latency. Actual
 * registry bookkeeping, projection bounds and atomic storage remain in use.
 */
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { mock } from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = process.argv[2] ? resolve(process.argv[2])
    : fileURLToPath(new URL('../../..', import.meta.url));
const load = path => import(pathToFileURL(join(root, path)).href);
const { Session } = await load('hub/src/state/registry.ts');
const { ConversationProjection } = await load('hub/src/subagents/conversation.ts');
const directory = mkdtempSync(join(tmpdir(), 'simplex-refresh-benchmark-'));
const part = raw => ({ type: 'text', modality: 'text', raw });
const session = new Session({ id: 'refresh-benchmark' });
session.kind = 'headless';
session.noteIdentity('worker');
session.workerCapabilities = { workerId: 'worker', names: ['session-history'] };
const queries = [];
const connection = { isOpen: true,
    sendPayload(payload) { queries.push(payload.data); return { ok: true }; } };
session.connection = connection;
const view = new ConversationProjection(session, join(directory, 'conversation.json'), 128 * 1024);
mock.timers.enable({ apis: ['setTimeout'] });
let sequence = 0;
let completedSteps = 0;
let answered = 0;
function event(name, data = {}) {
    const envelope = { event: name, worker_id: 'worker', request_id: 'task',
        run_id: 'run', sequence: ++sequence, data };
    session.noteEnvelope(envelope);
    view.event(envelope, connection);
}
function reply() {
    if (answered === queries.length) return;
    const query = queries[answered++];
    const indices = query.step === 0 ? [0]
        : Array.from({ length: completedSteps - query.step }, (_, index) => query.step + index);
    const nextStep = query.step === 0 && completedSteps > 1 ? 1 : 0;
    event('history', { request_id: query.request_id, revision: completedSteps,
        start: 0, step: query.step, total: 1, next: nextStep ? 0 : 1, next_step: nextStep,
        turns: [{ index: 0, user: [part('task')],
            omitted_steps: nextStep ? completedSteps - 1 : 0,
            steps: indices.map(index => ({ index, content: [part(`answer ${index}`)] })) }] });
}

try {
    view.connectionChanged(connection);
    mock.timers.tick(25);
    session.trackRequest('task', 'message');
    view.trackInput('task', [part('task')]);
    event('input_admitted');
    event('input_committed');
    for (let index = 0; index < 500; index += 1) {
        completedSteps += 1;
        event('model_response', { content: [part(`answer ${index}`)] });
        event('status', { active: true, capabilities: ['session-history'] });
        reply(); // Reply arrives after newer live commits, not before them.
        mock.timers.tick(25);
    }
    const queriesBeforeSettlement = queries.length;
    event('run_finished');
    for (let attempt = 0; attempt < 10 && view.value.stale; attempt += 1) {
        mock.timers.tick(25);
        reply();
    }
    assert.equal(view.value.stale, false);
    assert.equal(view.value.turns[0].steps.at(-1).content[0].raw, 'answer 499');
    console.log(JSON.stringify({ modelResponses: completedSteps, statusPolls: 500,
        queriesBeforeSettlement, queriesAfterSettlement: queries.length - queriesBeforeSettlement,
        totalQueries: queries.length, retainedSteps: view.value.turns[0].steps.length,
        current: !view.value.stale }, null, 2));
} finally {
    view.stop();
    mock.timers.reset();
    rmSync(directory, { recursive: true, force: true });
}
