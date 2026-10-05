/** Docker inspection/signaling must not silently change daemon after recovery. */
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { it } from 'node:test';
import { captureDockerManagement, dockerRunning, signalContainer, restoreDockerManagement } from '../src/subagents/docker.ts';
import { HubState } from '../src/state/persist.ts';
import { startTestHub } from './helpers/hub.js';
import { sessionDir } from '../src/launch/config-render.ts';

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
        host: process.env.DOCKER_HOST, context: process.env.DOCKER_CONTEXT, cwd: process.cwd() }) + '\\n');
    if (args[0] === 'inspect') console.log(state.running ? 'true' : 'false');
    if (args[0] === 'kill' && !state.ignore) { state.running = false; writeFileSync(path, JSON.stringify(state)); }
} catch { console.error('No such object: default daemon'); process.exitCode = 1; }
`, { mode: 0o755 });
    const invocation = { command: executable, args: ['run', '--name', 'test-child'], cwd: root,
        env: { DOCKER_CONFIG: directory, DOCKER_HOST: 'tcp://original-daemon:2376', DOCKER_CONTEXT: 'original-context' } };
    return { root, executable, directory, state, invocation };
}

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
