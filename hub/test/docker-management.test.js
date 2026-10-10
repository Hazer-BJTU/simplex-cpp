/** Docker inspection/signaling must not silently change daemon after recovery. */
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { it } from 'node:test';
import { captureDockerManagement, dockerRunning, signalContainer, restoreDockerManagement } from '../src/subagents/docker.ts';
import { HubState } from '../src/state/persist.ts';
import { startTestHub } from './helpers/hub.js';
import { sessionDir } from '../src/launch/config-render.ts';
import { until } from './helpers/worker.js';

function dockerFixture(t) {
    const root = mkdtempSync(join(tmpdir(), 'simplex-docker-context-'));
    t.after(() => rmSync(root, { recursive: true, force: true }));
    const executable = join(root, 'docker');
    const directory = join(root, 'daemon'); mkdirSync(directory);
    const state = join(directory, 'state.json');
    writeFileSync(state, JSON.stringify({ running: true, ignore: true }));
    writeFileSync(executable, `#!/usr/bin/env node
import { readFileSync, writeFileSync, appendFileSync } from 'node:fs';
const args = process.argv.slice(2);
const directory = process.env.DOCKER_CONFIG;
try {
    const path = directory + '/state.json';
    const state = JSON.parse(readFileSync(path, 'utf8'));
    appendFileSync(directory + '/calls.jsonl', JSON.stringify({ args, executable: process.argv[1],
        host: process.env.DOCKER_HOST, context: process.env.DOCKER_CONTEXT, cwd: process.cwd(),
        env: Object.fromEntries(['HOME', 'PATH', 'DOCKER_CERT_PATH', 'DOCKER_TLS_VERIFY',
            'SSH_AUTH_SOCK', 'HTTPS_PROXY', 'CUSTOM_MANAGER_TOKEN', 'MODEL_API_KEY',
            'DOCKER_UNRELATED_SECRET'].filter(key => process.env[key] !== undefined)
            .map(key => [key, process.env[key]])) }) + '\\n');
    if (args[0] === 'run') {
        writeFileSync(directory + '/startup.json', JSON.stringify({
            modelKey: process.env.MODEL_API_KEY, explicitKey: process.env.EXPLICIT_MODEL_KEY }));
        state.running = false; writeFileSync(path, JSON.stringify(state));
    }
    if (args[0] === 'inspect') {
        if (state.unknownInspections > 0) {
            state.unknownInspections--; writeFileSync(path, JSON.stringify(state));
            console.error('daemon temporarily unavailable'); process.exitCode = 1;
        } else console.log(state.running ? 'true' : 'false');
    }
    if (args[0] === 'kill' && !state.ignore) { state.running = false; writeFileSync(path, JSON.stringify(state)); }
} catch { console.error('No such object: default daemon'); process.exitCode = 1; }
`, { mode: 0o755 });
    const invocation = { command: executable, args: ['run', '--name', 'test-child'], cwd: root,
        env: { DOCKER_CONFIG: directory, DOCKER_HOST: 'tcp://original-daemon:2376', DOCKER_CONTEXT: 'original-context' } };
    return { root, executable, directory, state, invocation };
}

it('restores terminal state after a transient inspect failure without an adoption monitor', async t => {
    const fixture = dockerFixture(t);
    const ctx = await startTestHub({ worker: { stopTimeoutMs: 1, sigtermGraceMs: 1, sigkillGraceMs: 1 } });
    t.after(() => ctx.hub.stop());
    const session = ctx.hub.registry.create('docker-transient');
    ctx.hub.supervisor.adopt(session, { pid: 2147483647, pid_start_time: '123', command: fixture.executable,
        args: fixture.invocation.args, cwd: fixture.root, docker_management: captureDockerManagement(fixture.invocation) });
    const record = session.process;
    clearInterval(record.monitor);
    record.monitor = null;
    record.adopted = false;
    record.state = 'exited';
    writeFileSync(fixture.state, JSON.stringify({ running: false, ignore: true, unknownInspections: 1 }));
    assert.equal((await ctx.hub.supervisor.stop(session)).ok, true);
    assert.equal(record.state, 'exited');
    assert.equal(ctx.hub.supervisor.isRunning(session), false);
    assert.equal(record.monitor, null);
});

it('captures the startup executable/environment and restores them without Hub defaults', async t => {
    const fixture = dockerFixture(t);
    const context = captureDockerManagement(fixture.invocation);
    // This daemon reports absence even though the actual launch daemon is live.
    assert.equal(await dockerRunning({ ...context, env: { PATH: process.env.PATH } }), false);
    assert.equal(await dockerRunning(context), true);
    const saved = JSON.parse(JSON.stringify(context));
    const restored = restoreDockerManagement(saved, 'test-child');
    assert.equal(await dockerRunning(restored), true);
    await signalContainer(restored, 'TERM');
    assert.equal(await dockerRunning(restored), true);
    writeFileSync(fixture.state, JSON.stringify({ running: true, ignore: false }));
    await signalContainer(restored, 'KILL');
    assert.equal(await dockerRunning(restored), false);
    const calls = readFileSync(join(fixture.directory, 'calls.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
    for (const call of calls) {
        assert.equal(call.executable, fixture.executable);
        assert.equal(call.cwd, fixture.root);
        assert.equal(call.host, 'tcp://original-daemon:2376');
        assert.equal(call.context, 'original-context');
    }
    assert.equal(await dockerRunning(restoreDockerManagement(null, 'test-child')), null);
});

it('resolves Docker from startup PATH and retains that absolute executable for management', async t => {
    const fixture = dockerFixture(t);
    const invocation = { ...fixture.invocation, command: 'docker',
        env: { ...fixture.invocation.env, PATH: `${fixture.root}:${process.env.PATH}` } };
    const context = captureDockerManagement(invocation);
    assert.equal(context.executable, fixture.executable);
    assert.equal(context.env.PATH, invocation.env.PATH);
    const restored = restoreDockerManagement(JSON.parse(JSON.stringify(context)), 'test-child');
    assert.equal(await dockerRunning(restored), true);
    const calls = readFileSync(join(fixture.directory, 'calls.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
    assert.equal(calls[0].executable, fixture.executable);
});

it('keeps connection, TLS, SSH and proxy requirements while excluding unrelated secrets', async t => {
    const fixture = dockerFixture(t);
    const env = { ...fixture.invocation.env, HOME: fixture.root, PATH: process.env.PATH,
        DOCKER_CERT_PATH: './certs', DOCKER_TLS_VERIFY: '1', SSH_AUTH_SOCK: '/tmp/test-agent.sock',
        HTTPS_PROXY: 'http://test-proxy.invalid:3128', MODEL_API_KEY: 'model-secret-not-for-docker',
        DOCKER_UNRELATED_SECRET: 'not-a-management-variable', CUSTOM_MANAGER_TOKEN: 'explicit-manager-secret' };
    const invocation = { ...fixture.invocation, env };
    const context = captureDockerManagement(invocation, ['CUSTOM_MANAGER_TOKEN']);
    for (const key of ['HOME', 'PATH', 'DOCKER_CERT_PATH', 'DOCKER_TLS_VERIFY', 'SSH_AUTH_SOCK',
        'HTTPS_PROXY', 'CUSTOM_MANAGER_TOKEN']) assert.equal(context.env[key], env[key]);
    assert.equal(context.env.MODEL_API_KEY, undefined);
    assert.equal(context.env.DOCKER_UNRELATED_SECRET, undefined);
    assert.deepEqual(context.passThrough, ['CUSTOM_MANAGER_TOKEN']);
    // Environment values are never expanded or looked up again after capture.
    env.CUSTOM_MANAGER_TOKEN = 'changed-after-capture';
    const restored = restoreDockerManagement(JSON.parse(JSON.stringify(context)), 'test-child');
    assert.equal(await dockerRunning(restored), true);
    await signalContainer(restored, 'TERM');
    const calls = readFileSync(join(fixture.directory, 'calls.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
    for (const call of calls) {
        assert.equal(call.env.CUSTOM_MANAGER_TOKEN, 'explicit-manager-secret');
        assert.equal(call.env.DOCKER_CERT_PATH, './certs');
        assert.equal(call.env.SSH_AUTH_SOCK, '/tmp/test-agent.sock');
        assert.equal(call.env.HTTPS_PROXY, 'http://test-proxy.invalid:3128');
        assert.equal(call.env.MODEL_API_KEY, undefined);
        assert.equal(call.env.DOCKER_UNRELATED_SECRET, undefined);
    }
});

it('filters legacy full-environment snapshots without changing the saved daemon', async t => {
    const fixture = dockerFixture(t);
    const legacy = { executable: fixture.executable, cwd: fixture.root, name: 'test-child',
        env: { ...process.env, ...fixture.invocation.env,
            MODEL_API_KEY: 'legacy-secret', DOCKER_UNRELATED_SECRET: 'legacy-docker-secret' } };
    const saved = JSON.stringify(legacy);
    const restored = restoreDockerManagement(legacy, 'test-child');
    assert.equal(JSON.stringify(legacy), saved, 'restoration must not mutate its input');
    assert.equal(restored.env.MODEL_API_KEY, undefined);
    assert.equal(restored.env.DOCKER_UNRELATED_SECRET, undefined);
    assert.deepEqual(restored.passThrough, []);
    assert.equal(restored.executable, fixture.executable);
    assert.equal(restored.cwd, fixture.root);
    assert.equal(await dockerRunning(restored), true);
    const state = new HubState({ config: { dataDir: fixture.root }, log: { warn() {}, error() {} } });
    state.save([{ id: 'legacy', token: 'token', createdAt: '', process: {
        pid: 2147483647, pidStartTime: '123', startedAt: '', command: fixture.executable,
        args: fixture.invocation.args, cwd: fixture.root, state: 'running', dockerManagement: restored,
    } }]);
    assert.doesNotMatch(readFileSync(state.path, 'utf8'), /legacy-secret|legacy-docker-secret|MODEL_API_KEY|DOCKER_UNRELATED_SECRET/);
});

it('does not inherit newly added management values when they were absent at startup', async t => {
    const fixture = dockerFixture(t);
    const previous = process.env.DOCKER_CONFIG;
    process.env.DOCKER_CONFIG = fixture.directory;
    t.after(() => {
        if (previous === undefined) delete process.env.DOCKER_CONFIG;
        else process.env.DOCKER_CONFIG = previous;
    });
    const restored = restoreDockerManagement({ executable: fixture.executable, cwd: fixture.root,
        name: 'test-child', env: { PATH: process.env.PATH }, passThrough: ['CUSTOM_MANAGER_TOKEN'] }, 'test-child');
    assert.deepEqual(restored.env, { PATH: process.env.PATH });
    // Only retain PATH for the fixture interpreter. Inheriting this Hub's
    // DOCKER_CONFIG would query the live fixture daemon instead of reporting absence.
    assert.equal(await dockerRunning(restored), false);
});

it('rejects invalid persisted pass-through declarations instead of using Hub defaults', async t => {
    const fixture = dockerFixture(t);
    const context = captureDockerManagement(fixture.invocation);
    for (const passThrough of [null, 'CUSTOM_MANAGER_TOKEN', ['*'], ['A=B'], ['bad\0key'], ['A', 'A'], [3]]) {
        assert.equal(restoreDockerManagement({ ...context, passThrough }, 'test-child'), null);
        assert.throws(() => captureDockerManagement(fixture.invocation, passThrough), /environment names/);
    }
    assert.equal(restoreDockerManagement({ ...context, name: 'wrong-child' }, 'test-child'), null);
});

it('preserves full launch inheritance but persists only the selected Docker management environment', async t => {
    const fixture = dockerFixture(t);
    const previous = process.env.MODEL_API_KEY;
    process.env.MODEL_API_KEY = 'inherited-model-secret';
    t.after(() => {
        if (previous === undefined) delete process.env.MODEL_API_KEY;
        else process.env.MODEL_API_KEY = previous;
    });
    const ctx = await startTestHub({ launcher: { kind: 'command',
        command: [fixture.executable, 'run', '--name', 'test-child', '-e', 'MODEL_API_KEY'],
        dockerManagementEnv: ['CUSTOM_MANAGER_TOKEN'] } });
    try {
        const session = ctx.hub.registry.create('docker-inheritance', { env: {
            ...fixture.invocation.env, CUSTOM_MANAGER_TOKEN: 'configured-management-secret',
            EXPLICIT_MODEL_KEY: 'configured-model-secret',
        } });
        assert.equal((await ctx.hub.supervisor.start(session)).ok, true);
        await until(() => existsSync(join(fixture.directory, 'startup.json')));
        assert.deepEqual(JSON.parse(readFileSync(join(fixture.directory, 'startup.json'), 'utf8')),
            { modelKey: 'inherited-model-secret', explicitKey: 'configured-model-secret' });
        const context = session.process.dockerManagement;
        assert.equal(context.env.MODEL_API_KEY, undefined);
        assert.equal(context.env.EXPLICIT_MODEL_KEY, undefined);
        assert.equal(context.env.CUSTOM_MANAGER_TOKEN, 'configured-management-secret');
        const startup = JSON.parse(readFileSync(join(sessionDir(ctx.config, session.id), 'config', 'startup-launch.jsonc'), 'utf8'));
        assert.deepEqual(startup.launcher.dockerManagementEnv, ['CUSTOM_MANAGER_TOKEN']);
        const state = new HubState({ config: ctx.config, log: ctx.hub.supervisor.log });
        const ordinary = state.document([session]);
        const headless = state.sessionDocument({ ...session, kind: 'headless' });
        for (const snapshot of [ordinary, headless]) {
            assert.doesNotMatch(JSON.stringify(snapshot), /inherited-model-secret/);
            const stored = snapshot.sessions?.[0] ?? snapshot;
            assert.deepEqual(stored.process.docker_management, context);
        }
        assert.equal((await ctx.hub.supervisor.stop(session)).ok, true);
    } finally { await ctx.hub.stop(); }
});

it('retains adopted child storage until the original Docker daemon confirms termination', async t => {
    const fixture = dockerFixture(t);
    const ctx = await startTestHub({ worker: { bin: join(import.meta.dirname, 'fixtures/subagent-worker.js'),
        stopTimeoutMs: 10, sigtermGraceMs: 10, sigkillGraceMs: 10 } });
    t.after(() => ctx.hub.stop());
    const id = 'subagent-12345678-1234-4123-8123-123456789abc';
    const child = ctx.hub.registry.create(id); child.kind = 'headless';
    child.subagent = { parent: 'missing-parent', lifecycle: 'ready', health: 'unknown', policy: 'ask', reason: '', active: false, observed_at: null };
    const context = captureDockerManagement(fixture.invocation);
    const stored = { pid: 2147483647, pid_start_time: 'original-cli', command: fixture.executable,
        args: fixture.invocation.args, cwd: fixture.root, docker_management: context };
    assert.equal(ctx.hub.supervisor.adopt(child, stored), true);
    const root = sessionDir(ctx.config, id); mkdirSync(root, { recursive: true });
    writeFileSync(join(root, 'result.txt'), 'latest result');
    ctx.hub.subagents.children.set(id, { session: child, parent: { session_id: 'missing-parent', lifecycle_id: '', worker_id: '' },
        conversation: null, removed: false, startup: null, startupTimer: null, terminalTimer: null, uncertainStart: false, error: '' });
    const stopped = await ctx.hub.supervisor.stop(child);
    assert.equal(stopped.ok, false);
    assert.equal(child.subagent.lifecycle, 'cleanup-pending');
    assert.equal(readFileSync(join(root, 'result.txt'), 'utf8'), 'latest result');
    const state = new HubState({ config: ctx.config, log: ctx.hub.supervisor.log });
    const snapshot = state.document([{ id: 'ordinary', token: 'token', createdAt: '', process: child.process }]);
    assert.deepEqual(snapshot.sessions[0].process.docker_management, context);
    assert.doesNotMatch(JSON.stringify(child.process.describe()), /DOCKER_CONFIG|original-daemon|dockerManagement/);
    state.save([{ id: 'ordinary', token: 'token', createdAt: '', process: child.process }]);
    assert.equal(statSync(state.path).mode & 0o777, 0o600);
    // Loss of context must not turn default-daemon absence into termination.
    clearInterval(child.process.monitor);
    assert.equal(ctx.hub.supervisor.adopt(child, { ...stored, docker_management: undefined }), true);
    assert.equal((await ctx.hub.supervisor.stop(child)).ok, false);
    assert.equal(readFileSync(join(root, 'result.txt'), 'utf8'), 'latest result');
    clearInterval(child.process.monitor);
    assert.equal(ctx.hub.supervisor.adopt(child, stored), true);
    writeFileSync(fixture.state, JSON.stringify({ running: true, ignore: false }));
    assert.equal((await ctx.hub.supervisor.stop(child)).ok, true);
    assert.equal(child.subagent.lifecycle, 'stopped');
});

for (const state of ['exited', 'failed']) {
    it(`finishes ${state} CLI records after container termination and includes them in stopAll`, async t => {
        const fixture = dockerFixture(t);
        const ctx = await startTestHub({ worker: { stopTimeoutMs: 1, sigtermGraceMs: 1, sigkillGraceMs: 1 } });
        t.after(() => ctx.hub.stop());
        const session = ctx.hub.registry.create(`docker-${state}`);
        assert.equal(ctx.hub.supervisor.adopt(session, {
            pid: 2147483647, pid_start_time: '123', command: fixture.executable,
            args: fixture.invocation.args, cwd: fixture.root,
            docker_management: captureDockerManagement(fixture.invocation),
        }), true);
        const record = session.process;
        ctx.hub.supervisor.finish(record, state === 'failed' ? { error: 'CLI failed' } : { exitCode: 0 });
        // Exit notification begins family cleanup; it must first fail while the
        // daemon says the container ignores signals, leaving a stopping record.
        await ctx.hub.supervisor.stop(session);
        assert.equal(record.state, 'stopping');
        assert.equal(await dockerRunning(record.dockerManagement), true);
        // A later CLI-terminal observation must not exclude a live container
        // from Hub shutdown's final stopAll pass.
        record.state = state;
        writeFileSync(fixture.state, JSON.stringify({ running: true, ignore: false }));
        const results = await ctx.hub.supervisor.stopAll();
        assert.equal(results.find(result => result.session === session.id)?.ok, true);
        assert.equal(await dockerRunning(record.dockerManagement), false);
        assert.equal(record.state, 'exited');
        assert.equal(record.monitor, null);
    });
}
