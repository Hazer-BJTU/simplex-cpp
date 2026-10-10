/** Deterministic write/fsync/broadcast counts and lifetime boundaries for event bursts. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { basename, join } from 'node:path';
import { it } from 'node:test';
import { SessionRegistry } from '../src/state/registry.ts';
import { SubagentService } from '../src/subagents/service.ts';
import { ConversationProjection } from '../src/subagents/conversation.ts';
import { ProjectionFlush, PROJECTION_FLUSH_MS } from '../src/subagents/projection-flush.ts';
import { sessionDir } from '../src/launch/config-render.ts';
import { createLogger } from '../src/log.ts';
import { testConfig } from './helpers/hub.js';

const part = raw => ({ type: 'text', modality: 'text', raw });

/** Instrument the real atomic writer, keeping actual fsyncs and disk publications. */
function writes(t) {
    const counts = { fsyncs: 0, files: [] };
    const fsync = fs.fsyncSync;
    const rename = fs.renameSync;
    t.mock.method(fs, 'fsyncSync', fd => { counts.fsyncs += 1; return fsync(fd); });
    t.mock.method(fs, 'renameSync', (from, to) => {
        const result = rename(from, to);
        counts.files.push(basename(to));
        return result;
    });
    syncBuiltinESMExports();
    t.after(() => { t.mock.restoreAll(); syncBuiltinESMExports(); });
    counts.reset = () => { counts.fsyncs = 0; counts.files.length = 0; };
    return counts;
}

/** Controlled event channel; existing subagents.test.js covers real process/socket startup. */
function fixture(t) {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const config = testConfig();
    const log = createLogger({ level: 'silent' });
    const registry = new SessionRegistry({ config, log });
    const parent = registry.create('parent');
    const session = registry.create('subagent-12345678-1234-4123-8123-123456789abc');
    session.kind = 'headless';
    session.subagent = { parent: parent.id, lifecycle: 'ready', policy: 'ask',
        health: 'healthy', reason: '', observed_at: null, active: true };
    session.activeRunId = 'run';
    session.noteIdentity('worker');
    session.workerCapabilities = { workerId: 'worker', names: [] };
    const connection = { session, isOpen: true, terminate() {} };
    session.connection = connection;
    let broadcasts = 0;
    const supervisor = {
        isRunning: () => false,
        stopProcess: async () => ({ ok: true, how: 'not-started', forced: false }),
        stop: current => supervisor.cascade(current, () => supervisor.stopProcess(current)),
    };
    const service = new SubagentService({ config, registry, supervisor, log,
        changed: () => { broadcasts += 1; }, removed() {} });
    const root = sessionDir(config, session.id);
    const conversation = new ConversationProjection(session, join(root, 'conversation.json'),
        config.subagents.conversationBytes, () => service.projectionStorageFailed(session));
    const record = { session, conversation, parent: { session_id: parent.id,
        lifecycle_id: parent.lifecycleId, worker_id: 'parent-worker' },
        removed: false, startup: null, startupTimer: null, terminalTimer: null,
        error: '', uncertainStart: false, terminationConfirmed: true, cleanupAttempts: 0, retryAt: 0 };
    service.children.set(session.id, record);
    service.publish(record);
    session.trackRequest('request', 'message');
    service.operations.set(session.id, { receipts: [], requests: [{ request_id: 'request',
        operation: 'message', state: 'sent', run_id: '', at: 'fixture' }] });
    const counts = writes(t);
    let sequence = 0;
    const event = (name = 'tool_calls', data = {}) => service.onEvent({ event: name,
        worker_id: 'worker', request_id: 'request', run_id: 'run',
        sequence: ++sequence, received_at: `observation-${sequence}`, data }, connection);
    t.after(async () => {
        await service.shutdown();
        t.mock.timers.reset();
        fs.rmSync(config.dataDir, { recursive: true, force: true });
    });
    return { config, root, session, record, service, conversation, counts, event,
        broadcasts: () => broadcasts, reset: () => { broadcasts = 0; counts.reset(); } };
}

it('coalesces 100 model steps and list broadcasts while retaining the latest bounded dialogue', t => {
    const ctx = fixture(t);
    ctx.reset();
    ctx.conversation.trackInput('request', [part('task')]);
    ctx.event('input_committed');
    for (let index = 0; index < 100; index += 1) {
        ctx.event('model_response', { content: [part(`answer ${index}`)] });
        ctx.event('tool_calls');
    }
    assert.equal(ctx.counts.fsyncs, 0, 'noncritical events do no synchronous disk writes');
    assert.equal(ctx.broadcasts(), 0);
    assert.equal(ctx.conversation.value.turns[0].steps.at(-1).content[0].raw, 'answer 99');
    assert.equal(ctx.conversation.value.turns[0].steps.length, 32, 'memory bounds apply before disk flush');
    t.mock.timers.tick(PROJECTION_FLUSH_MS);
    assert.deepEqual(ctx.counts.files.sort(), ['conversation.json', 'metadata.json']);
    assert.equal(ctx.counts.fsyncs, 2);
    assert.equal(ctx.broadcasts(), 1);
    const persisted = JSON.parse(fs.readFileSync(join(ctx.root, 'conversation.json'), 'utf8'));
    assert.deepEqual(persisted, ctx.conversation.value);
    const metadata = JSON.parse(fs.readFileSync(join(ctx.root, 'metadata.json'), 'utf8'));
    assert.equal(metadata.subagent.observed_at, 'observation-201');
    t.mock.timers.tick(PROJECTION_FLUSH_MS * 2);
    assert.equal(ctx.counts.fsyncs, 2, 'idle windows do not rewrite projections');
});

it('does not postpone projection deadlines under continuous events', t => {
    const ctx = fixture(t);
    ctx.reset();
    for (let index = 0; index < 12; index += 1) {
        ctx.event();
        t.mock.timers.tick(PROJECTION_FLUSH_MS / 4);
    }
    assert.equal(ctx.counts.fsyncs, 3);
    assert.equal(ctx.broadcasts(), 3);
    assert.equal(JSON.parse(fs.readFileSync(join(ctx.root, 'metadata.json'), 'utf8'))
        .subagent.observed_at, 'observation-12');
});

it('preserves synchronous operation outcomes, operator policy and ready transitions', t => {
    const ctx = fixture(t);
    ctx.reset();
    ctx.event('input_admitted');
    assert.equal(JSON.parse(fs.readFileSync(join(ctx.root, 'operations.json'), 'utf8'))
        .requests[0].state, 'admitted');
    for (let index = 0; index < 30; index += 1) ctx.event();
    ctx.service.setPolicy(ctx.session, 'deny');
    assert.equal(JSON.parse(fs.readFileSync(join(ctx.root, 'metadata.json'), 'utf8')).subagent.policy, 'deny');
    t.mock.timers.tick(PROJECTION_FLUSH_MS);
    assert.equal(ctx.counts.files.filter(name => name === 'metadata.json').length, 1,
        'critical publication supersedes the queued observation');
    ctx.record.session.subagent.lifecycle = 'starting';
    ctx.event('ready');
    assert.equal(JSON.parse(fs.readFileSync(join(ctx.root, 'metadata.json'), 'utf8')).subagent.lifecycle, 'ready');
    ctx.event('run_finished', { status: 'completed' });
    assert.equal(JSON.parse(fs.readFileSync(join(ctx.root, 'operations.json'), 'utf8'))
        .requests[0].state, 'finished');
    assert.equal(ctx.counts.files.filter(name => name === 'operations.json').length, 2,
        'only changed outcomes rewrite the durable ledger');
});

it('joins pending dialogue and cancels metadata observations before stop deletes storage', async t => {
    const ctx = fixture(t);
    ctx.reset();
    ctx.conversation.trackInput('request', [part('last task')]);
    ctx.event('input_committed');
    ctx.event('model_response', { content: [part('last answer')] });
    assert.equal((await ctx.service.options.supervisor.stop(ctx.session)).ok, true);
    assert.equal(ctx.counts.files.filter(name => name === 'conversation.json').length, 1);
    assert.equal(fs.existsSync(ctx.root), false);
    const before = ctx.counts.fsyncs;
    t.mock.timers.tick(PROJECTION_FLUSH_MS * 3);
    ctx.event();
    assert.equal(ctx.counts.fsyncs, before);
    assert.equal(fs.existsSync(ctx.root), false);
});

it('reports deferred failures and continues cleanup without recreating removed storage', async t => {
    const ctx = fixture(t);
    ctx.reset();
    const backup = `${ctx.root}-backup`;
    fs.renameSync(ctx.root, backup);
    fs.symlinkSync(backup, ctx.root, 'dir');
    ctx.event();
    t.mock.timers.tick(PROJECTION_FLUSH_MS);
    assert.equal(ctx.session.subagent.health, 'degraded');
    assert.match(ctx.session.subagent.reason, /metadata storage/);
    fs.unlinkSync(ctx.root);
    fs.renameSync(backup, ctx.root);
    ctx.event();
    t.mock.timers.tick(PROJECTION_FLUSH_MS);
    ctx.event();
    assert.equal(ctx.session.subagent.health, 'healthy', 'new observations recover after storage repairs');
    ctx.conversation.trackInput('request', [part('pending before failure')]);
    ctx.event('input_committed');
    const open = fs.openSync;
    t.mock.method(fs, 'openSync', (path, ...args) => {
        if (String(path).includes('conversation.json')) throw new Error('injected write failure');
        return open(path, ...args);
    });
    syncBuiltinESMExports();
    assert.equal((await ctx.service.options.supervisor.stop(ctx.session)).ok, true);
    assert.equal(ctx.conversation.storageFailed, true);
    assert.equal(fs.existsSync(ctx.root), false, 'projection failure must not skip lifetime cleanup');
    t.mock.timers.tick(PROJECTION_FLUSH_MS * 3);
    assert.equal(fs.existsSync(ctx.root), false);
});

it('flushes shutdown projections while retaining durable evidence of an unconfirmed stop', async t => {
    const ctx = fixture(t);
    ctx.reset();
    ctx.record.terminationConfirmed = false;
    ctx.service.options.supervisor.stopProcess = async session => ({
        ok: session.kind !== 'headless', how: 'injected-stop-failure', forced: false,
    });
    ctx.conversation.trackInput('request', [part('retained task')]);
    ctx.event('input_committed');
    ctx.event('model_response', { content: [part('retained answer')] });
    await ctx.service.shutdown();
    const dialogue = JSON.parse(fs.readFileSync(join(ctx.root, 'conversation.json'), 'utf8'));
    assert.equal(dialogue.turns[0].steps[0].content[0].raw, 'retained answer');
    const metadata = JSON.parse(fs.readFileSync(join(ctx.root, 'metadata.json'), 'utf8'));
    assert.equal(metadata.subagent.lifecycle, 'cleanup-pending');
    assert.equal(metadata.cleanup_attempts, 2, 'parent cascade and final orphan cleanup both retain ownership');
    assert.equal(metadata.termination_confirmed, false);
    const before = ctx.counts.fsyncs;
    t.mock.timers.tick(PROJECTION_FLUSH_MS * 3);
    assert.equal(ctx.counts.fsyncs, before, 'shutdown joins projection work even when ownership is retained');
});

it('contains failing observers and closes the scheduler before its final attempt', t => {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    let writes = 0;
    let reports = 0;
    const task = new ProjectionFlush(() => { writes += 1; throw new Error('storage'); }, () => {
        reports += 1;
        task.schedule();
        throw new Error('observer');
    });
    task.schedule();
    assert.equal(task.stop(), false);
    t.mock.timers.tick(PROJECTION_FLUSH_MS * 2);
    task.schedule();
    assert.equal(writes, 1);
    assert.equal(reports, 1);
});
