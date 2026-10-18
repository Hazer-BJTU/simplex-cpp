/** Real worker + manager: SQLite startup, read-only inspection, compact and restart. */
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { it } from 'node:test';
import { tmpdir } from 'node:os';
import { WORKER_BIN, e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';
import { ConfigurationStore } from '../../src/configurations/store.ts';
import { snapshotConfigs } from '../../src/configurations/session.ts';
import { normalizeSpec } from '../../src/launch/spec.ts';
import { renderWorkerConfig } from '../../src/launch/config-render.ts';
import { readManagedSession } from '../../src/launch/managed-sessions.ts';

it('managed SQLite state survives compact and restart without JSON side copies',
    { skip: e2eSkip, timeout: 90000 }, async t => {
        const ctx = await startE2eHub({ worker: { bin: WORKER_BIN,
            persistence: { enabled: true, readable: false, backend: 'sqlite' } } });
        const targetRoot = mkdtempSync(join(tmpdir(), 'simplex-sqlite-target-'));
        t.after(async () => {
            await ctx.hub.stop();
            rmSync(ctx.config.dataDir, { recursive: true, force: true });
            rmSync(targetRoot, { recursive: true, force: true });
        });
        const session = ctx.hub.registry.create('sqlite-managed', { provider: 'mock', model: 'mock-text' });
        const document = renderWorkerConfig({ config: ctx.config, sessionId: session.id,
            spec: normalizeSpec(ctx.config, { provider: 'mock', model: 'mock-text', modalityAssistProvider: null }),
            endpoints: ctx.hub.supervisor.endpointsFor(session.id, session.token),
            mock: { baseUrl: ctx.hub.mock.baseUrl } });
        const configs = new ConfigurationStore(ctx.config);
        configs.save('worker', 'sqlite-e2e', JSON.stringify(document), null);
        configs.save('launch', 'sqlite-e2e', JSON.stringify({ launcher: { kind: 'command', command: ['unused'] },
            management: { target: { kind: 'local' }, executable: join(dirname(WORKER_BIN), 'simplex'),
                cwd: dirname(WORKER_BIN), env: {}, root: targetRoot },
            worker: { threads: 1, stopTimeoutMs: 15000 } }), null);
        snapshotConfigs(configs, session.id, { launchConfig: 'sqlite-e2e', workerConfig: 'sqlite-e2e' });
        const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
        t.after(() => panel.close());
        const started = await ctx.hub.supervisor.start(session);
        assert.equal(started.ok, true, started.error);
        await until(() => session.workerCapabilities?.names.includes('context-compact'), { timeout: 20000 });
        panel.send({ v: 1, type: 'subscribe', session: session.id });
        await panel.waitFor(value => value.type === 'subscribed');
        panel.send({ v: 1, type: 'input', session: session.id, request_id: 'task',
            content: [{ type: 'text', modality: 'text', raw: 'Remember this task. '.repeat(5000) }] });
        const finished = await panel.waitFor(value => value.type === 'event'
            && value.envelope.event === 'run_finished' && value.envelope.request_id === 'task', { timeout: 20000 });
        assert.equal(finished.envelope.data.status, 'completed');
        const before = await ctx.hub.supervisor.managedSnapshot(session);
        assert.equal(before.backend, 'sqlite');
        assert.equal(before.state.turns.length, 1);
        panel.send({ v: 1, type: 'input', session: session.id, request_id: 'compact', operation: 'compact' });
        const compact = await panel.waitFor(value => value.type === 'event'
            && value.envelope.event === 'compact_finished', { timeout: 20000 });
        assert.equal(compact.envelope.data.archive.storage_kind, 'sqlite');
        assert.equal(compact.envelope.data.memory_file, undefined);
        await panel.waitFor(value => value.type === 'event' && value.envelope.event === 'run_finished'
            && value.envelope.request_id === 'compact', { timeout: 20000 });
        const after = await ctx.hub.supervisor.managedSnapshot(session);
        assert.deepEqual(after.state.turns, []);
        const allocation = readManagedSession(ctx.config, session.id);
        assert.equal(existsSync(join(allocation.reference.directory, 'state/state.sqlite3')), true);
        assert.equal(existsSync(join(allocation.reference.directory, 'state/state.json')), false);
        assert.equal((await ctx.hub.supervisor.stop(session)).ok, true);
        assert.equal((await ctx.hub.supervisor.start(session)).ok, true);
        await until(() => session.connection?.isOpen, { timeout: 20000 });
        const restarted = await ctx.hub.supervisor.managedSnapshot(session);
        assert.equal(restarted.backend, 'sqlite');
        assert.deepEqual(restarted.state.turns, []);
        assert.deepEqual(restarted.state.system_prompt, after.state.system_prompt);
    });
