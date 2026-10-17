import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, existsSync, readFileSync, writeFileSync, rmSync, symlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { HubState } from '../src/state/persist.ts';
import { createLogger } from '../src/log.ts';
import { startTestHub } from './helpers/hub.js';
import { ConfigurationStore, launchDocument } from '../src/configurations/store.ts';
import { snapshotConfigs } from '../src/configurations/session.ts';
import { readManagedSession, separateLocalRoot, managedRecordPath } from '../src/launch/managed-sessions.ts';

// Explicit shutdown inside a recovery test and teardown share the same promise.
// In particular, never poll an unconfirmed remote dispatch after HTTP handles
// have already closed: unref'ed polling timers cannot keep Node 22 alive alone.
const stoppedHubs = new WeakMap();
function stopHub(hub) {
    if (!stoppedHubs.has(hub)) stoppedHubs.set(hub, hub.stop());
    return stoppedHubs.get(hub);
}

const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function until(predicate) {
    const end = Date.now() + 4000;
    while (!predicate()) {
        if (Date.now() > end) throw new Error('fixture condition timed out');
        await pause(10);
    }
}

async function setup(t, remote = false) {
    const temp = mkdtempSync(join(tmpdir(), 'simplex-managed-sessions-'));
    const dataDir = join(temp, 'hub');
    const target = join(temp, 'target-emulator');
    mkdirSync(target);
    const executable = join(temp, 'simplex.mjs');
    writeFileSync(executable, `#!${process.execPath}\nimport ${JSON.stringify(new URL('./fixtures/managed-target.mjs', import.meta.url).href)};\n`, { mode: 0o700 });
    const context = await startTestHub({ dataDir, worker: { stopTimeoutMs: 80, sigtermGraceMs: 1000, sigkillGraceMs: 1000 } });
    const extraHubs = [];
    t.after(async () => {
        for (const extra of extraHubs) await stopHub(extra);
        // Explicit operator cleanup of fixture remote workers, outside Hub policy.
        const dispatched = join(target, 'dispatched.pid');
        if (existsSync(dispatched) && existsSync(join(target, 'hold-start'))) {
            try { process.kill(Number(readFileSync(dispatched, 'utf8')), 'SIGTERM'); } catch {}
        }
        if (existsSync(join(target, 'last-run.json'))) {
            let record;
            try { record = readManagedSession(context.config, 'demo'); } catch {}
            if (record) {
                const lease = join(target, `${record.reference.allocationId}.lease`);
                if (existsSync(lease)) try { process.kill(Number(readFileSync(lease, 'utf8')), 'SIGTERM'); } catch {}
            }
        }
        await stopHub(context.hub);
        rmSync(temp, { recursive: true, force: true });
    });
    let transport = { kind: 'local' };
    if (remote) {
        const command = join(temp, 'docker.mjs');
        writeFileSync(command, `#!${process.execPath}\nimport {spawn} from 'node:child_process';
            const args=process.argv.slice(2); if(args[0]!=='exec'||args[1]!=='retained')process.exit(99);
            const child=spawn(args[2],args.slice(3),{stdio:'inherit'});
            child.on('exit',code=>{process.exitCode=code??1;});
        `, { mode: 0o700 });
        transport = { kind: 'docker', command, args: [], container: 'retained' };
    }
    const store = new ConfigurationStore(context.config);
    const launch = { launcher: { kind: 'command', command: ['ignored-legacy-command'] },
        worker: { threads: 2, stopTimeoutMs: remote ? 500 : 80, sigtermGraceMs: 1000, sigkillGraceMs: 1000 },
        management: { target: transport, executable, cwd: temp, env: { MANAGED_FIXTURE_ROOT: target },
            root: remote ? '/only-on-target/allocations' : join(temp, 'allocations') } };
    store.save('launch', 'managed', JSON.stringify(launch), null);
    snapshotConfigs(store, 'demo', { launchConfig: 'managed', workerConfig: 'default' });
    const session = context.hub.registry.create('demo', {});
    const api = async (path, method = 'GET', body) => {
        const response = await fetch(context.base + path, { method, headers: { 'Content-Type': 'application/json' },
            ...(body === undefined ? {} : { body: JSON.stringify(body) }) });
        return { status: response.status, body: await response.json() };
    };
    const source = readFileSync(join(dataDir, 'sessions/demo/config/config.yaml'), 'utf8');
    return { ...context, temp, target, session, store, launch, api, source, extraHubs };
}

function operations(ctx) {
    return readFileSync(join(ctx.target, 'operations.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
}

async function start(ctx) {
    const result = await ctx.api('/api/sessions/demo/start', 'POST', {});
    assert.equal(result.body.ok, true, JSON.stringify(result));
    await until(() => existsSync(join(ctx.target, 'last-run.json')));
    return readManagedSession(ctx.config, 'demo');
}

test('managed local sessions prepare/capture/run, read uncapped snapshots, restart from saved source and delete only their allocation', async t => {
    const ctx = await setup(t);
    const record = await start(ctx);
    assert.notEqual(record.reference.directory, join(ctx.config.dataDir, 'sessions/demo'));
    assert.equal(existsSync(record.reference.directory), false, 'fixture target is not a Hub filesystem path');
    assert.equal(record.configuration, ctx.source);
    assert.equal(ctx.session.spec.modalityAssistProvider, null);
    assert.equal(ctx.session.spec.threads, 2);
    const args = JSON.parse(readFileSync(join(ctx.target, 'last-run.json')));
    assert.equal(args[2], `${record.reference.directory}/config/config.yaml`);
    const snapshot = await ctx.api('/api/sessions/demo/snapshot');
    assert.equal(snapshot.status, 200);
    assert.equal(snapshot.body.state.text.length, 10 * 1024 * 1024);
    assert.deepEqual(snapshot.body.files, {});
    const stop = await ctx.api('/api/sessions/demo/stop', 'POST');
    assert.equal(stop.body.ok, true);
    // Mutating or deleting original library entries cannot change the captured source/target.
    ctx.store.remove('launch', 'managed', ctx.store.read('launch', 'managed').revision);
    ctx.store.remove('worker', 'default', ctx.store.read('worker', 'default').revision);
    const restart = await ctx.api('/api/sessions/demo/start', 'POST', {});
    assert.equal(restart.body.ok, true);
    const restarted = readManagedSession(ctx.config, 'demo');
    assert.equal(restarted.reference.allocationId, record.reference.allocationId);
    assert.equal(restarted.configuration, ctx.source);
    assert.notEqual(restarted.lifecycle.id, record.lifecycle.id);
    await ctx.api('/api/sessions/demo/stop', 'POST');
    const unrelated = join(ctx.config.dataDir, 'unrelated.json');
    writeFileSync(unrelated, 'retained');
    const removed = await ctx.api('/api/sessions/demo', 'DELETE');
    assert.equal(removed.status, 200);
    assert.equal(readFileSync(unrelated, 'utf8'), 'retained');
    assert.equal(JSON.parse(readFileSync(join(ctx.target, `${record.reference.allocationId}.json`))).status, 'removed');
    assert.equal(ctx.hub.registry.get('demo'), undefined);
});

test('uncertain prepare and remove retain the same durable reference and reconcile on retry', async t => {
    const ctx = await setup(t);
    writeFileSync(join(ctx.target, 'drop-prepare'), '');
    const failed = await ctx.api('/api/sessions/demo/start', 'POST', {});
    assert.equal(failed.body.ok, false);
    const record = readManagedSession(ctx.config, 'demo');
    assert.ok(record);
    assert.equal(existsSync(join(ctx.target, 'last-run.json')), false);
    await start(ctx);
    await ctx.api('/api/sessions/demo/stop', 'POST');
    writeFileSync(join(ctx.target, 'drop-remove'), '');
    assert.equal((await ctx.api('/api/sessions/demo', 'DELETE')).status, 500);
    assert.equal(readManagedSession(ctx.config, 'demo').removing, true);
    assert.ok(ctx.hub.registry.get('demo'));
    assert.equal((await ctx.api('/api/sessions/demo/start', 'POST', {})).body.ok, false);
    assert.equal((await ctx.api('/api/sessions/demo', 'DELETE')).status, 200);
    assert.deepEqual(new Set(operations(ctx).map(op => op.allocation_id)), new Set([record.reference.allocationId]));
});

test('manager failures never fall back to a local state file or silently succeed with empty state', async t => {
    const ctx = await setup(t);
    await start(ctx);
    const fakeState = join(ctx.config.dataDir, 'sessions/demo/state');
    mkdirSync(fakeState);
    writeFileSync(join(fakeState, 'state.json'), '{"local":"must not be read"}');
    writeFileSync(join(ctx.target, 'invalid-state'), '');
    const reply = await ctx.api('/api/sessions/demo/snapshot');
    assert.equal(reply.status, 500);
    assert.equal(reply.body.state, undefined);
    assert.equal(reply.body.error, 'internal_error');
});

test('new async prepare holds admission against start, stop, configuration replacement and delete', async t => {
    const ctx = await setup(t);
    writeFileSync(join(ctx.target, 'delay-prepare'), '');
    const starting = ctx.api('/api/sessions/demo/start', 'POST', {});
    await until(() => existsSync(join(ctx.target, 'operations.jsonl')));
    assert.equal((await ctx.api('/api/sessions/demo/start', 'POST', {})).body.ok, false);
    assert.equal((await ctx.api('/api/sessions/demo/stop', 'POST')).body.ok, false);
    assert.equal((await ctx.api('/api/sessions/demo', 'DELETE')).status, 409);
    assert.equal((await ctx.api('/api/sessions/demo/configurations', 'POST', { launchConfig: 'local', workerConfig: 'default' })).status, 409);
    assert.equal((await starting).body.ok, true);
});

test('managed target/source cannot be silently overwritten and old worker roots are never adopted', async t => {
    const ctx = await setup(t);
    await start(ctx);
    await ctx.api('/api/sessions/demo/stop', 'POST');
    assert.throws(() => snapshotConfigs(ctx.store, 'demo', { launchConfig: 'local', workerConfig: 'default' }, true), /immutable/);
    const root = join(ctx.config.dataDir, 'sessions/old');
    mkdirSync(root, { recursive: true });
    writeFileSync(join(root, 'session.lock'), 'legacy');
    assert.throws(() => snapshotConfigs(ctx.store, 'old', { launchConfig: 'managed', workerConfig: 'default' }, true), /migration/);
    const policy = ctx.launch.management;
    for (const path of [ctx.config.dataDir, join(ctx.config.dataDir, 'worker'), ctx.temp]) {
        assert.throws(() => separateLocalRoot(ctx.config, { ...policy, root: path }), /separate/);
    }
    const alias = join(ctx.temp, 'hub-alias');
    symlinkSync(ctx.config.dataDir, alias);
    assert.throws(() => launchDocument(JSON.stringify({ ...ctx.launch, management: { ...policy, root: join(alias, 'worker') } }), ctx.config), /separate/);
});

test('corrupt managed reference fails closed for inspection and deletion instead of selecting legacy paths', async t => {
    const ctx = await setup(t);
    writeFileSync(managedRecordPath(ctx.config, 'demo'), '{');
    const snapshot = await ctx.api('/api/sessions/demo/snapshot');
    assert.equal(snapshot.status, 500);
    const deletion = await ctx.api('/api/sessions/demo', 'DELETE');
    assert.equal(deletion.status, 500);
    assert.ok(ctx.hub.registry.get('demo'));
    assert.ok(existsSync(join(ctx.config.dataDir, 'sessions/demo/config/config.yaml')));
});

test('retained-container target supports snapshots without a shared path and refuses host-side signal escalation', async t => {
    const ctx = await setup(t, true);
    const record = await start(ctx);
    assert.equal((await ctx.api('/api/sessions/demo/snapshot')).body.state.text.length, 10 * 1024 * 1024);
    const stopped = await ctx.api('/api/sessions/demo/stop', 'POST');
    assert.equal(stopped.body.ok, false);
    assert.equal(stopped.body.how, 'remote-ownership-unconfirmed');
    assert.equal((await ctx.api('/api/sessions/demo/force-kill', 'POST')).body.ok, false);
    const pid = Number(readFileSync(join(ctx.target, `${record.reference.allocationId}.lease`), 'utf8'));
    process.kill(pid, 0);
    process.kill(pid, 'SIGTERM');
    await until(() => !existsSync(join(ctx.target, `${record.reference.allocationId}.lease`)));
    assert.equal((await ctx.api('/api/sessions/demo/stop', 'POST')).body.ok, true);
    assert.equal((await ctx.api('/api/sessions/demo', 'DELETE')).status, 200);
    assert.ok(operations(ctx).some(op => op.operation === 'remove'));
});

test('cleanup timeout retains local records for retry', async t => {
    const ctx = await setup(t);
    await start(ctx);
    await ctx.api('/api/sessions/demo/stop', 'POST');
    const record = readManagedSession(ctx.config, 'demo');
    record.timeoutMs = 50;
    writeFileSync(managedRecordPath(ctx.config, 'demo'), JSON.stringify(record));
    writeFileSync(join(ctx.target, 'delay-prepare'), '');
    const deletion = await ctx.api('/api/sessions/demo', 'DELETE');
    assert.equal(deletion.status, 500);
    assert.equal(readManagedSession(ctx.config, 'demo').removing, true);
    assert.ok(ctx.hub.registry.get('demo'));
});

test('Hub shutdown waits for an in-flight start before stopping the resulting worker', async t => {
    const ctx = await setup(t);
    writeFileSync(join(ctx.target, 'delay-prepare'), '');
    const starting = ctx.hub.supervisor.start(ctx.session);
    await until(() => existsSync(join(ctx.target, 'operations.jsonl')));
    const stopping = ctx.hub.supervisor.stopAll();
    assert.equal((await starting).ok, true);
    assert.ok((await stopping).every(result => result.ok));
    const record = readManagedSession(ctx.config, 'demo');
    assert.equal(existsSync(join(ctx.target, `${record.reference.allocationId}.lease`)), false);
    assert.equal((await ctx.hub.supervisor.start(ctx.session)).ok, false);
});

test('managed launch template is available through the existing configuration editor API', async t => {
    const ctx = await setup(t);
    const reply = await ctx.api('/api/configurations/launch/template?source=managed');
    assert.equal(reply.status, 200);
    const launch = launchDocument(reply.body.text, ctx.config);
    assert.equal(launch.management.target.kind, 'local');
    assert.ok(launch.management.root.startsWith('/'));
    assert.equal(launch.management.timeoutMs, 30000);
    assert.notEqual(launch.management.root, ctx.config.dataDir);
});

test('a restarted Hub reuses the durable allocation and source after library deletion', async t => {
    const ctx = await setup(t);
    const before = await start(ctx);
    await ctx.api('/api/sessions/demo/stop', 'POST');
    ctx.store.remove('launch', 'managed', ctx.store.read('launch', 'managed').revision);
    await stopHub(ctx.hub);
    const restored = await startTestHub({ dataDir: ctx.config.dataDir });
    ctx.extraHubs.push(restored.hub);
    const session = restored.hub.registry.get('demo');
    assert.ok(session);
    assert.equal((await restored.hub.supervisor.start(session)).ok, true);
    const after = readManagedSession(restored.config, 'demo');
    assert.equal(after.reference.allocationId, before.reference.allocationId);
    assert.equal(after.configuration, ctx.source);
    assert.equal(after.launcher.executable, before.launcher.executable);
    assert.equal((await restored.hub.supervisor.managedSnapshot(session)).state.text.length, 10 * 1024 * 1024);
    assert.equal((await restored.hub.supervisor.stop(session)).ok, true);
    // Stop before setup's teardown removes shared fixture storage.
    await stopHub(restored.hub);
});

test('remote CLI death is not worker death and manager refusal retains cleanup ownership', async t => {
    const ctx = await setup(t, true);
    const allocation = await start(ctx);
    const record = ctx.session.process;
    process.kill(record.pid, 'SIGKILL');
    await until(() => record.child.signalCode === 'SIGKILL');
    assert.equal((await ctx.api('/api/sessions/demo/stop', 'POST')).body.ok, false);
    const deletion = await ctx.api('/api/sessions/demo', 'DELETE');
    assert.equal(deletion.status, 500);
    assert.ok(ctx.hub.registry.get('demo'));
    assert.equal(readManagedSession(ctx.config, 'demo').reference.allocationId, allocation.reference.allocationId);
    const lease = join(ctx.target, `${allocation.reference.allocationId}.lease`);
    const pid = Number(readFileSync(lease, 'utf8'));
    process.kill(pid, 0);
    process.kill(pid, 'SIGTERM');
    await until(() => !existsSync(lease));
    assert.equal((await ctx.api('/api/sessions/demo/stop', 'POST')).body.ok, false);
    assert.equal((await ctx.api('/api/sessions/demo', 'DELETE')).status, 200);
});

test('an idle probe cannot confirm stop while a remote command is dispatched before lease acquisition', async t => {
    const ctx = await setup(t, true);
    const barrier = join(ctx.target, 'hold-start');
    writeFileSync(barrier, '');
    assert.equal((await ctx.api('/api/sessions/demo/start', 'POST', {})).body.ok, true);
    await until(() => existsSync(join(ctx.target, 'dispatched.pid')));
    const record = readManagedSession(ctx.config, 'demo');
    assert.equal(record.lifecycle.dispatch, 'pending');
    const lease = join(ctx.target, `${record.reference.allocationId}.lease`);
    assert.equal(existsSync(lease), false);
    const stopped = await ctx.api('/api/sessions/demo/stop', 'POST');
    assert.equal(stopped.body.ok, false);
    assert.equal(stopped.body.how, 'remote-ownership-unconfirmed');
    assert.notEqual(ctx.session.process.state, 'exited');
    assert.equal((await ctx.api('/api/sessions/demo/start', 'POST', {})).body.ok, false);
    rmSync(barrier);
    await until(() => existsSync(lease));
    process.kill(Number(readFileSync(lease, 'utf8')), 'SIGTERM');
    await until(() => readManagedSession(ctx.config, 'demo').lifecycle.dispatch === 'completed');
    assert.equal((await ctx.api('/api/sessions/demo/stop', 'POST')).body.ok, true);
});

test('explicit deletion fences a delayed remote start even after the local transport is lost', async t => {
    const ctx = await setup(t, true);
    const barrier = join(ctx.target, 'hold-start');
    writeFileSync(barrier, '');
    assert.equal((await ctx.api('/api/sessions/demo/start', 'POST', {})).body.ok, true);
    await until(() => existsSync(join(ctx.target, 'dispatched.pid')));
    const record = readManagedSession(ctx.config, 'demo');
    process.kill(ctx.session.process.pid, 'SIGKILL');
    assert.equal((await ctx.api('/api/sessions/demo', 'DELETE')).status, 200);
    rmSync(barrier);
    const remotePid = Number(readFileSync(join(ctx.target, 'dispatched.pid'), 'utf8'));
    await until(() => { try { process.kill(remotePid, 0); return false; } catch { return true; } });
    assert.equal(existsSync(join(ctx.target, `${record.reference.allocationId}.lease`)), false);
    assert.equal(JSON.parse(readFileSync(join(ctx.target, `${record.reference.allocationId}.json`))).status, 'removed');
});

test('recovery reconciles durable dispatch without a process entry or connected worker', async t => {
    const ctx = await setup(t, true);
    // Save exactly the registry document that predates process publication.
    new HubState({ config: ctx.config, log: createLogger({ level: 'silent' }) }).flush(ctx.hub.registry.list());
    const registryPath = join(ctx.config.dataDir, 'hub.json');
    const beforeDispatch = readFileSync(registryPath);
    const allocation = await start(ctx);
    process.kill(ctx.session.process.pid, 'SIGKILL');
    await until(() => ctx.session.process.child.signalCode === 'SIGKILL');
    await stopHub(ctx.hub);
    writeFileSync(registryPath, beforeDispatch);
    const restored = await startTestHub({ dataDir: ctx.config.dataDir });
    ctx.extraHubs.push(restored.hub);
    const session = restored.hub.registry.get('demo');
    assert.equal(session.connected, false);
    assert.ok(session.process, 'recovery must create an unconfirmed ownership record without a stored PID');
    assert.equal(session.process.pid, null);
    assert.equal(session.lifecycleId, allocation.lifecycle.id);
    const count = operations(ctx).length;
    const outcomes = await restored.hub.supervisor.stopAll({ timeoutMs: 500 });
    assert.equal(outcomes.length, 1);
    assert.equal(outcomes[0].session, 'demo');
    assert.equal(outcomes[0].ok, false);
    assert.ok(operations(ctx).slice(count).some(op => op.operation === 'capture_startup'));
    assert.equal((await restored.hub.supervisor.start(session)).ok, false);
    const lease = join(ctx.target, `${allocation.reference.allocationId}.lease`);
    process.kill(Number(readFileSync(lease, 'utf8')), 'SIGTERM');
    await until(() => !existsSync(lease));
    // Repeated idle results cannot turn a lost dispatch into confirmed completion.
    assert.equal((await restored.hub.supervisor.stop(session, { timeoutMs: 150 })).ok, false);
    const response = await fetch(restored.base + '/api/sessions/demo', { method: 'DELETE' });
    assert.equal(response.status, 200);
    await stopHub(restored.hub);
});

for (const identity of ['matching', 'stale-lifecycle', 'unknown', 'missing']) {
    test(`local recovery after an offline exit: ${identity} process snapshot`, { skip: process.platform !== 'linux' }, async t => {
        const ctx = await setup(t);
        await start(ctx);
        new HubState({ config: ctx.config, log: createLogger({ level: 'silent' }) }).flush(ctx.hub.registry.list());
        const registryPath = join(ctx.config.dataDir, 'hub.json');
        const snapshot = JSON.parse(readFileSync(registryPath, 'utf8'));
        const allocationPath = managedRecordPath(ctx.config, 'demo');
        const pending = readFileSync(allocationPath);
        // Stop the fixture, then restore the two documents as they would have
        // remained if the worker exited while the Hub was offline.
        await stopHub(ctx.hub);
        const entry = snapshot.sessions.find(session => session.id === 'demo');
        if (identity === 'stale-lifecycle') entry.lifecycle_id = 'older-lifecycle';
        if (identity === 'unknown') entry.process.pid_start_time = null;
        if (identity === 'missing') entry.process = null;
        writeFileSync(registryPath, JSON.stringify(snapshot));
        writeFileSync(allocationPath, pending);
        const restored = await startTestHub({ dataDir: ctx.config.dataDir, worker: { stopTimeoutMs: 100 } });
        ctx.extraHubs.push(restored.hub);
        const session = restored.hub.registry.get('demo');
        assert.equal(session.process, null);
        const dispatch = readManagedSession(ctx.config, 'demo').lifecycle.dispatch;
        if (identity === 'matching') {
            assert.equal(dispatch, 'completed');
            assert.equal((await restored.hub.supervisor.start(session)).ok, true);
            assert.equal((await restored.hub.supervisor.stop(session)).ok, true);
        } else {
            assert.equal(dispatch, 'pending');
            assert.equal((await restored.hub.supervisor.start(session)).ok, false);
            assert.equal((await restored.hub.supervisor.stop(session, { timeoutMs: 100 })).ok, false);
        }
    });
}
