/** Durable cleanup policy across Hub startup and recursive timer retries. */
import assert from 'node:assert/strict';
import { existsSync, readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { it } from 'node:test';
import { hubRoot } from '../src/config.ts';
import { createHub } from '../src/hub.ts';
import { sessionDir } from '../src/launch/config-render.ts';
import { createLogger } from '../src/log.ts';
import { writePrivate } from '../src/subagents/storage.ts';
import { testConfig } from './helpers/hub.js';
import { until } from './helpers/worker.js';

const parent = { session_id: 'recovery-parent', lifecycle_id: 'parent-lifecycle', worker_id: 'parent-worker' };
const ids = [1, 2, 3].map(index => `subagent-12345678-1234-4123-8123-123456789ab${index}`);

/** Seed an ordinary parent's durable stop intent without starting any process. */
function recoveryHub(t) {
    const config = testConfig();
    writePrivate(join(config.dataDir, 'hub.json'), {
        version: 1,
        sessions: [{ id: parent.session_id, token: 'parent-token', spec: {},
            lifecycle_id: parent.lifecycle_id,
            process: { pid: 2147483647, pid_start_time: '123', command: '/bin/true',
                args: [], cwd: config.dataDir, state: 'stopping' } }],
    }, 512 * 1024);
    const hub = createHub({ config, hubRoot, log: createLogger({ level: 'silent' }), version: 'test' });
    const stopProcess = hub.supervisor.stopProcess.bind(hub.supervisor);
    t.after(async () => {
        // Cleanup is an explicit shutdown; it must not affect measured automatic attempts.
        hub.supervisor.stopProcess = stopProcess;
        await hub.stop();
        rmSync(config.dataDir, { recursive: true, force: true });
    });
    return { hub, config };
}

function seedChild(config, id, owner = parent, overrides = {}) {
    const path = join(sessionDir(config, id), 'metadata.json');
    writePrivate(path, {
        id, kind: 'headless', token: `token-${id}`, lifecycle_id: `lifecycle-${id}`,
        cascading_parent: owner, process: null,
        subagent: { policy: 'ask', lifecycle: 'preparing' },
        ...overrides,
    }, 512 * 1024);
    return path;
}

it('does not bypass an exhausted descendant budget when restoring an ordinary parent stop', async t => {
    const { hub, config } = recoveryHub(t);
    const path = seedChild(config, ids[0], parent, {
        termination_confirmed: true, cleanup_attempts: 3,
        subagent: { policy: 'ask', lifecycle: 'cleanup-pending' },
    });
    const before = readFileSync(path, 'utf8');
    await hub.start();
    const child = hub.registry.require(ids[0]);
    assert.equal(hub.registry.require(parent.session_id).closing, true);
    assert.equal(child.subagent.lifecycle, 'cleanup-pending');
    assert.equal(hub.subagents.recoveryState(child).cleanup_attempts, 3);
    assert.match(child.subagent.reason, /cleanup paused/);
    assert.equal(readFileSync(path, 'utf8'), before);
    assert.equal((await hub.subagents.recover(child, 'retry', child.lifecycleId)).ok, true);
    assert.equal(existsSync(sessionDir(config, child.id)), false);
});

it('preserves persisted backoff through startup and the ordinary parent stop cascade', async t => {
    const { hub, config } = recoveryHub(t);
    const retryAt = Date.now() + 60000;
    const path = seedChild(config, ids[0], parent, {
        termination_confirmed: true, cleanup_attempts: 2, retry_at: retryAt,
        subagent: { policy: 'ask', lifecycle: 'cleanup-pending' },
    });
    const before = readFileSync(path, 'utf8');
    await hub.start();
    const child = hub.registry.require(ids[0]);
    hub.subagents.retryCleanup(retryAt - 1);
    assert.equal(hub.subagents.stops.has(child.id), false);
    assert.equal(child.subagent.lifecycle, 'cleanup-pending');
    assert.equal(readFileSync(path, 'utf8'), before);
    const recovery = hub.subagents.recoveryState(child);
    assert.equal(recovery.cleanup_attempts, 2);
    assert.equal(recovery.retry_at, new Date(retryAt).toISOString());

    // The same durable attempt becomes eligible at the deadline, without an operator reset.
    hub.subagents.retryCleanup(retryAt);
    await until(() => !hub.subagents.stops.has(child.id));
    assert.equal(child.subagent.lifecycle, 'stopped');
    assert.equal(existsSync(sessionDir(config, child.id)), false);
});

for (const order of ['ancestor-first', 'descendant-first']) {
    it(`attempts each nested child once per automatic pass (${order})`, async t => {
        const { hub, config } = recoveryHub(t);
        // Directory enumeration is lexical; reverse the graph to exercise both traversal orders.
        const chain = order === 'ancestor-first' ? ids : [...ids].reverse();
        for (const [index, id] of chain.entries()) {
            const owner = index === 0 ? parent : { session_id: chain[index - 1],
                lifecycle_id: `lifecycle-${chain[index - 1]}`, worker_id: `worker-${index}` };
            seedChild(config, id, owner);
        }
        const attempts = new Map(ids.map(id => [id, 0]));
        const original = hub.supervisor.stopProcess.bind(hub.supervisor);
        hub.supervisor.stopProcess = async session => {
            if (session.kind !== 'headless') return original(session);
            attempts.set(session.id, attempts.get(session.id) + 1);
            return { ok: false, how: 'injected-cleanup-failure', forced: false };
        };
        const startedAt = Date.now();
        await hub.start();
        for (const id of ids) {
            assert.equal(attempts.get(id), 1, id);
            const metadata = JSON.parse(readFileSync(join(sessionDir(config, id), 'metadata.json'), 'utf8'));
            assert.equal(metadata.cleanup_attempts, 1, id);
            assert.ok(metadata.retry_at >= startedAt + 10000, id);
        }

        // A timer pass also shares its attempt set across recursive and top-level traversal.
        // Advancing its clock past newly computed deadlines must not duplicate an attempt.
        for (const expected of [2, 3]) {
            hub.subagents.retryCleanup(Date.now() + 3600000);
            await until(() => hub.subagents.stops.size === 0);
            for (const id of ids) {
                assert.equal(attempts.get(id), expected, id);
                assert.equal(hub.subagents.children.get(id).cleanupAttempts, expected, id);
            }
        }
        const snapshots = ids.map(id => readFileSync(join(sessionDir(config, id), 'metadata.json'), 'utf8'));
        hub.subagents.retryCleanup(Date.now() + 3600000);
        assert.equal(hub.subagents.stops.size, 0);
        for (const [index, id] of ids.entries()) {
            assert.equal(attempts.get(id), 3, id);
            assert.equal(readFileSync(join(sessionDir(config, id), 'metadata.json'), 'utf8'), snapshots[index]);
        }
    });
}
