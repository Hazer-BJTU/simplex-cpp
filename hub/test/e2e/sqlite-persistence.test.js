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
import { loadConfig, hubRoot, parseConfigText } from '../../src/config.ts';
import { createHub } from '../../src/hub.ts';
import { createLogger } from '../../src/log.ts';

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

// Use production defaults rather than the general E2E helper's explicit legacy
// JSON policy. The worker binary and offline provider are the only overrides.

for (const mode of ['api-default', 'bundled-local', 'api-json']) {
    it(`Inspector reads a completed real-worker run through ${mode}`,
        { skip: e2eSkip, timeout: 60000 }, async t => {
            const dataDir = mkdtempSync(join(tmpdir(), 'simplex-default-inspection-'));
            const { config } = loadConfig({ overrides: {
                dataDir, listen: { host: '127.0.0.1', port: 0 }, toolRequests: { port: 0 },
                worker: { bin: WORKER_BIN }, mock: { enabled: true, slowMs: 2000 },
            } });
            assert.equal(config.worker.persistence.backend, 'sqlite');
            const hub = createHub({ config, hubRoot, version: 'test', log: createLogger({ level: 'silent' }) });
            const address = await hub.start();
            const base = `http://127.0.0.1:${address.port}`;
            t.after(async () => {
                await hub.stop();
                rmSync(dataDir, { recursive: true, force: true });
                rmSync(`${dataDir}-workers`, { recursive: true, force: true });
            });
            const api = async (path, method = 'GET', body) => {
                const response = await fetch(`${base}${path}`, { method,
                    ...(body === undefined ? {} : { headers: { 'content-type': 'application/json' },
                        body: JSON.stringify(body) }) });
                const value = await response.json();
                assert.ok(response.ok, JSON.stringify(value));
                return value;
            };
            let spec = { provider: 'mock', model: mode === 'api-default' ? 'mock-slow' : 'mock-text',
                modalityAssistProvider: null, threads: 2 };
            if (mode === 'bundled-local') {
                const configs = new ConfigurationStore(config);
                const local = configs.read('launch', 'local');
                const launch = parseConfigText(local.text, 'local template');
                // Only choose the installed executable; retain the shipped
                // template's management policy and root/cwd settings.
                launch.management.executable = join(dirname(WORKER_BIN), 'simplex');
                configs.save('launch', 'local', JSON.stringify(launch), local.revision);
                const document = renderWorkerConfig({ config, sessionId: mode,
                    spec: normalizeSpec(config, spec),
                    endpoints: { events: 'ws://localhost/events', confirm: 'ws://localhost/confirm', tools: 'ws://localhost/tools' },
                    mock: { baseUrl: hub.mock.baseUrl } });
                configs.save('worker', 'offline', JSON.stringify(document), null);
                spec = { launchConfig: 'local', workerConfig: 'offline' };
            } else if (mode === 'api-json') {
                spec.persistence = { backend: 'json' };
            }
            await api('/api/sessions', 'POST', { session: mode, spec });
            const started = await api(`/api/sessions/${mode}/start`, 'POST', {});
            assert.equal(started.ok, true, started.error);
            const session = hub.registry.get(mode);
            await until(() => session.workerCapabilities?.names.includes('context-compact'), { timeout: 20000 });
            const panel = await connectWorker(`ws://127.0.0.1:${address.port}/panel/ws`);
            t.after(() => panel.close());
            panel.send({ v: 1, type: 'subscribe', session: mode });
            await panel.waitFor(value => value.type === 'subscribed');
            panel.send({ v: 1, type: 'input', session: mode, request_id: 'task',
                content: [{ type: 'text', modality: 'text', raw: 'A normal saved conversation.' }] });
            let childAllocation;
            if (mode === 'api-default') {
                await until(() => session.activeRunId, { timeout: 5000 });
                const url = new URL(hub.supervisor.endpointsFor(mode, session.token).tools);
                url.pathname += '/subagent/clean-fork';
                const remote = await connectWorker(url.href);
                t.after(() => remote.close());
                remote.send({ type: 'tool_request', data: { worker_id: session.identity.workerId,
                    session_id: mode, run_id: session.activeRunId, request_id: 'fork', arguments: {} } });
                const response = (await remote.waitFor(value => value.type === 'tool_response')).data;
                assert.equal(response.status, 'succeeded', JSON.stringify(response));
                const child = hub.registry.require(response.result.subagent_id);
                await until(() => child.subagent.lifecycle === 'ready', { timeout: 15000 });
                childAllocation = readManagedSession(config, child.id);
                assert.ok(childAllocation);
                const childState = await hub.supervisor.managedSnapshot(child);
                assert.equal(childState.backend, 'sqlite');
                assert.equal(childState.state?.turns.length ?? 0, 0);
            }
            const finished = await panel.waitFor(value => value.type === 'event'
                && value.envelope.event === 'run_finished' && value.envelope.request_id === 'task', { timeout: 20000 });
            assert.equal(finished.envelope.data.status, 'completed');
            const snapshot = await api(`/api/sessions/${mode}/snapshot`);
            assert.ok(snapshot.state, snapshot.state_error);
            assert.equal(snapshot.state.meta.session_id, mode);
            assert.equal(snapshot.state.turns.length, 1);
            const allocation = readManagedSession(config, mode);
            if (mode === 'api-json') {
                assert.equal(allocation, null, 'explicit JSON retains legacy API launch compatibility');
                assert.equal(session.spec.persistence.backend, 'json');
            } else {
                assert.equal(snapshot.backend, 'sqlite');
                assert.equal(session.spec.persistence.backend, 'sqlite');
                assert.ok(allocation);
                assert.equal(existsSync(join(allocation.reference.directory, 'state/state.sqlite3')), true);
                assert.equal(existsSync(join(dataDir, 'sessions', mode, 'state')), false);
                assert.equal((await api(`/api/sessions/${mode}/stop`, 'POST', {})).ok, true);
                if (childAllocation) assert.equal(existsSync(childAllocation.reference.directory), false);
                assert.equal((await api(`/api/sessions/${mode}/start`, 'POST', {})).ok, true);
                await until(() => session.connection?.isOpen, { timeout: 20000 });
                const restarted = await api(`/api/sessions/${mode}/snapshot`);
                assert.deepEqual(restarted.state.turns, snapshot.state.turns);
                assert.equal(readManagedSession(config, mode).reference.directory, allocation.reference.directory);
            }
        });
}
