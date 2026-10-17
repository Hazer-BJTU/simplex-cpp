/** Opt-in real manager/worker acceptance on an operator-provisioned Docker or SSH target. */
import assert from 'node:assert/strict';
import { existsSync, rmSync } from 'node:fs';
import { randomUUID } from 'node:crypto';
import { it } from 'node:test';
import { startTestHub } from '../helpers/hub.js';
import { connectWorker, until } from '../helpers/worker.js';
import { ConfigurationStore } from '../../src/configurations/store.ts';
import { snapshotConfigs } from '../../src/configurations/session.ts';
import { renderWorkerConfig, sessionDir } from '../../src/launch/config-render.ts';
import { normalizeSpec } from '../../src/launch/spec.ts';
import { readManagedSession } from '../../src/launch/managed-sessions.ts';
import { ManagedLauncher } from '../../src/launch/managed-launcher.ts';

const configured = process.env.SIMPLEX_MANAGED_E2E_POLICY;
it('real target clean-fork isolates state and removes nested children without shared Hub storage',
    { skip: configured ? false : 'set SIMPLEX_MANAGED_E2E_POLICY to opt in', timeout: 90000 }, async t => {
        const policy = JSON.parse(configured);
        const ctx = await startTestHub({ mock: { enabled: true, slowMs: 10000 },
            worker: { threads: 1, stopTimeoutMs: 15000, sigtermGraceMs: 1000, sigkillGraceMs: 1000 } });
        t.after(async () => { await ctx.hub.stop(); rmSync(ctx.config.dataDir, { recursive: true, force: true }); });
        const parent = ctx.hub.registry.create(`managed-${randomUUID()}`);
        const source = renderWorkerConfig({ config: ctx.config, sessionId: parent.id,
            spec: normalizeSpec(ctx.config, { provider: 'mock', model: 'mock-slow', modalityAssistProvider: null }),
            endpoints: ctx.hub.supervisor.endpointsFor(parent.id, parent.token), mock: { baseUrl: ctx.hub.mock.baseUrl } });
        const store = new ConfigurationStore(ctx.config);
        store.save('worker', 'managed-e2e', JSON.stringify(source), null);
        store.save('launch', 'managed-e2e', JSON.stringify({ launcher: { kind: 'command', command: ['unused'] },
            management: policy, worker: { threads: 1, stopTimeoutMs: 15000 } }), null);
        snapshotConfigs(store, parent.id, { launchConfig: 'managed-e2e', workerConfig: 'managed-e2e' });
        const started = await ctx.hub.supervisor.start(parent);
        assert.equal(started.ok, true, started.error);
        await until(() => parent.workerCapabilities?.names.includes('session-history'), { timeout: 15000 })
            .catch(error => { throw new Error(`${error.message}\n${ctx.hub.supervisor.logs(parent).join('\n')}`); });
        const parentAllocation = readManagedSession(ctx.config, parent.id);
        assert.equal(existsSync(parentAllocation.reference.directory), false, 'target path must not exist on Hub host');
        const message = session => session.connection.sendPayload({ type: 'payload', data: {
            operation: 'message', request_id: `message-${randomUUID()}`,
            content: [{ type: 'text', modality: 'text', raw: 'Give a short answer for the offline test.' }],
        } });
        const fork = async caller => {
            const url = new URL(ctx.hub.supervisor.endpointsFor(caller.id, caller.token).tools);
            url.pathname += '/subagent/clean-fork';
            const peer = await connectWorker(url.href);
            peer.send({ type: 'tool_request', data: { worker_id: caller.identity.workerId, session_id: caller.id,
                run_id: caller.activeRunId, request_id: `fork-${randomUUID()}`, arguments: {} } });
            const response = (await peer.waitFor(value => value.type === 'tool_response')).data;
            await peer.waitForClose();
            assert.equal(response.status, 'succeeded', JSON.stringify(response));
            const child = ctx.hub.registry.require(response.result.subagent_id);
            await until(() => child.subagent.lifecycle === 'ready', { timeout: 15000 });
            return child;
        };
        message(parent);
        await until(() => parent.activeRunId, { timeout: 5000 });
        const child = await fork(parent);
        const childAllocation = readManagedSession(ctx.config, child.id);
        const inspect = record => new ManagedLauncher(record.launcher).manage(record.reference, randomUUID(), { operation: 'inspect' });
        const description = (await inspect(childAllocation)).result;
        assert.equal(description.effective_configuration.persistence.directory, childAllocation.reference.directory);
        assert.equal(description.effective_configuration.persistence.restore, 'if_present');
        assert.equal(child.subagent.policy, 'ask');
        message(child);
        await until(() => child.activeRunId, { timeout: 5000 });
        const grandchild = await fork(child);
        const grandAllocation = readManagedSession(ctx.config, grandchild.id);
        await until(() => ctx.hub.subagents.children.get(child.id).conversation.value.turns[0]?.steps.length
            && !child.activeRunId, { timeout: 20000 });
        const snapshot = await ctx.hub.supervisor.managedSnapshot(child);
        assert.ok(snapshot.state, 'Inspector must read the actual persisted state over target stdio');
        assert.equal(existsSync(sessionDir(ctx.config, child.id) + '/state'), false);
        assert.equal((await ctx.hub.supervisor.stop(parent)).ok, true);
        for (const [session, allocation] of [[child, childAllocation], [grandchild, grandAllocation]]) {
            assert.equal((await inspect(allocation)).result.status, 'removed');
            assert.equal(existsSync(sessionDir(ctx.config, session.id)), false);
        }
        // Retained Docker/SSH environment remains reachable after worker shutdown.
        assert.equal((await inspect(parentAllocation)).result.status, 'ready');
        await ctx.hub.supervisor.removeStorage(parent, () => rmSync(sessionDir(ctx.config, parent.id), { recursive: true, force: true }));
        assert.equal((await inspect(parentAllocation)).result.status, 'removed');
    });
