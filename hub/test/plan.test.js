import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, it } from 'node:test';
import { PlanStore } from '../src/state/plan.ts';
import { startTestHub } from './helpers/hub.js';
import { connectWorker, workerEvent, until } from './helpers/worker.js';
import { createPanelStore } from '../web/src/state/store.ts';

const cleanup = [];
afterEach(async () => { for (const close of cleanup.splice(0).reverse()) await close(); });

it('persists replacements, treats identical text as a no-op, and preserves malformed storage', () => {
    const root = mkdtempSync(join(tmpdir(), 'simplex-plan-'));
    cleanup.push(() => rmSync(root, { recursive: true, force: true }));
    const plans = new PlanStore(root);
    assert.equal(plans.read('demo').markdown, '');
    const first = plans.replace('demo', '- [ ] Task');
    assert.equal(first.changed, true);
    assert.equal(first.plan.revision, 1);
    assert.equal(new PlanStore(root).read('demo').markdown, '- [ ] Task');
    assert.equal(plans.replace('demo', '- [ ] Task').changed, false);
    assert.equal(plans.replace('demo', ' \n ').plan.markdown, '');
    assert.equal(plans.read('other').markdown, '');
    assert.throws(() => plans.replace('demo', 'x'.repeat(65537)));
    assert.throws(() => plans.replace('demo', '\ud800'));
    const path = join(root, 'sessions/demo/plan.json');
    writeFileSync(path, 'broken');
    assert.throws(() => plans.replace('demo', 'replacement'));
    assert.equal(readFileSync(path, 'utf8'), 'broken');
    // An invalid destination must not get silently replaced with a new plan.
    rmSync(path);
    mkdirSync(path);
    assert.throws(() => plans.replace('demo', 'replacement'));
});

async function setup() {
    const ctx = await startTestHub({ limits: { confirmIdentityHoldMs: 120 } });
    cleanup.push(() => ctx.hub.http.server.listening ? ctx.hub.stop() : undefined);
    const session = ctx.hub.registry.create('demo', {});
    const endpoints = ctx.hub.supervisor.endpointsFor(session.id, session.token);
    const event = await connectWorker(endpoints.events);
    cleanup.push(() => event.close());
    let sequence = 0;
    const emit = (name, run = 'run-1', data = {}) => event.send(workerEvent({
        session: 'demo', worker: 'worker-1', sequence: ++sequence, event: name, runId: run, data,
    }));
    const call = async (operation, args = {}, identity = {}) => {
        const url = new URL(endpoints.tools);
        url.pathname += `/plan/${operation}`;
        const peer = await connectWorker(url.href);
        peer.send({ type: 'tool_request', data: {
            worker_id: 'worker-1', session_id: 'demo', run_id: 'run-1', request_id: `req-${Math.random()}`,
            arguments: { operation, ...args }, ...identity,
        } });
        const response = await peer.waitFor((value) => value.type === 'tool_response');
        await peer.waitForClose();
        return response.data;
    };
    return { ...ctx, session, endpoints, event, emit, call };
}

it('waits for run admission, broadcasts only durable changes, and restores the subscription snapshot', async () => {
    const ctx = await setup();
    const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
    cleanup.push(() => panel.close());
    await panel.waitFor((m) => m.type === 'welcome');
    panel.send({ type: 'subscribe', session: 'demo' });
    const initial = await panel.waitFor((m) => m.type === 'subscribed');
    assert.equal(initial.plan.markdown, '');
    const pending = ctx.call('replace', { markdown: '- [ ] Task' });
    await new Promise((resolve) => setTimeout(resolve, 30));
    const queuedRead = ctx.call('read');
    ctx.emit('input_admitted');
    const result = await pending;
    assert.equal((await queuedRead).result.markdown, '- [ ] Task');
    assert.equal(result.status, 'succeeded');
    assert.equal(result.result.revision, 1);
    assert.equal(Object.hasOwn(result.result, 'markdown'), false);
    const update = await panel.waitFor((m) => m.type === 'plan');
    assert.equal(update.plan.markdown, '- [ ] Task');
    assert.equal(new PlanStore(ctx.config.dataDir).read('demo').revision, 1);
    assert.equal((await ctx.call('replace', { markdown: '- [ ] Task' })).result.changed, false);
    assert.equal((await ctx.call('read')).result.markdown, '- [ ] Task');
    const resumed = await connectWorker(`${ctx.wsBase}/panel/ws`);
    cleanup.push(() => resumed.close());
    resumed.send({ type: 'subscribe', session: 'demo' });
    assert.equal((await resumed.waitFor((m) => m.type === 'subscribed')).plan.revision, 1);
    assert.equal((await ctx.call('replace', { markdown: '' })).result.revision, 2);
    await panel.waitFor((m) => m.type === 'plan' && m.plan.markdown === '');
});

it('rejects stale identities, finished runs and bad arguments without changing the plan', async () => {
    const ctx = await setup();
    ctx.emit('input_admitted');
    await until(() => ctx.session.activeRunId === 'run-1');
    assert.equal((await ctx.call('replace', { markdown: 'ok' })).status, 'succeeded');
    assert.equal((await ctx.call('replace', { markdown: 'bad' }, { worker_id: 'old' })).error.code, 'unauthorized');
    assert.equal((await ctx.call('read', { markdown: '' })).error.code, 'invalid_arguments');
    assert.equal((await ctx.call('replace', { markdown: 1 })).error.code, 'invalid_arguments');
    assert.equal((await ctx.call('replace', { markdown: 'x'.repeat(65537) })).error.code, 'invalid_arguments');
    ctx.emit('run_finished');
    await until(() => ctx.session.activeRunId === '');
    assert.equal((await ctx.call('replace', { markdown: 'bad' })).error.code, 'unauthorized');
    assert.equal(new PlanStore(ctx.config.dataDir).read('demo').markdown, 'ok');
});

it('does not commit a request disconnected while waiting for identity', async () => {
    const ctx = await setup();
    const url = new URL(ctx.endpoints.tools);
    url.pathname += '/plan/replace';
    const peer = await connectWorker(url.href);
    peer.send({ type: 'tool_request', data: {
        worker_id: 'worker-1', session_id: 'demo', run_id: 'run-1', request_id: 'gone',
        arguments: { operation: 'replace', markdown: 'must not commit' },
    } });
    await peer.close();
    ctx.emit('input_admitted');
    await new Promise((resolve) => setTimeout(resolve, 40));
    assert.equal(new PlanStore(ctx.config.dataDir).read('demo').revision, 0);
});

it('panel plan state ignores stale updates but accepts an authoritative restart snapshot', () => {
    const store = createPanelStore();
    const plan = (revision, markdown) => ({ revision, markdown, updated_at: null });
    store.getState().setPlan('a', plan(5, 'latest'));
    store.getState().setPlan('a', plan(4, 'stale'));
    store.getState().setPlan('b', plan(1, 'other'));
    assert.equal(store.getState().plans.get('a').markdown, 'latest');
    store.getState().setPlan('a', plan(0, ''), true);
    assert.equal(store.getState().plans.get('a').markdown, '');
    store.getState().removeSession('b');
    assert.equal(store.getState().plans.has('b'), false);
});


it('restores a saved plan after hub restart without a connected worker', async () => {
    const ctx = await setup();
    ctx.emit('input_admitted');
    await until(() => ctx.session.activeRunId === 'run-1');
    assert.equal((await ctx.call('replace', { markdown: 'Retained plan' })).status, 'succeeded');
    await ctx.hub.stop();
    const restarted = await startTestHub({ dataDir: ctx.config.dataDir });
    cleanup.push(() => restarted.hub.stop());
    const panel = await connectWorker(`${restarted.wsBase}/panel/ws`);
    cleanup.push(() => panel.close());
    panel.send({ type: 'subscribe', session: 'demo' });
    const snapshot = await panel.waitFor((m) => m.type === 'subscribed');
    assert.equal(snapshot.session.connected, false);
    assert.equal(snapshot.plan.markdown, 'Retained plan');
});

it('reports storage failure without publishing a plan update', async () => {
    const ctx = await setup();
    ctx.emit('input_admitted');
    await until(() => ctx.session.activeRunId === 'run-1');
    const root = join(ctx.config.dataDir, 'sessions/demo');
    mkdirSync(root, { recursive: true });
    const path = join(root, 'plan.json');
    writeFileSync(path, 'invalid existing document');
    assert.equal((await ctx.call('replace', { markdown: 'new' })).error.code, 'storage_error');
    assert.equal(readFileSync(path, 'utf8'), 'invalid existing document');
});
