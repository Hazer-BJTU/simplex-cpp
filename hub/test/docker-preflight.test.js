/** Docker startup checks: read-only, ordered, and never a container lifetime command. */
import assert from 'node:assert/strict';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { it } from 'node:test';
import { DockerPreflightError, preflightDockerTarget } from '../src/launch/docker-preflight.ts';
import { normalizeDockerPreflight } from '../src/launch/managed-launcher.ts';
import { managementPolicy, readManagedSession } from '../src/launch/managed-sessions.ts';
import { ConfigurationStore } from '../src/configurations/store.ts';
import { snapshotConfigs } from '../src/configurations/session.ts';
import { startTestHub } from './helpers/hub.js';

const endpoints = {
    events: 'ws://127.0.0.1:8800/worker/events?token=secret-token',
    confirm: 'ws://127.0.0.1:8800/worker/confirm?token=secret-token',
    tools: 'ws://127.0.0.1:8801/worker/tools?token=secret-token',
};

const defaults = {
    enabled: true, timeoutMs: 5000, container: true, executable: true,
    endpoint: { enabled: true, channels: ['events'] },
};

/** One fake Docker CLI plus the state it exposes through its environment. */
function dockerFixture(t, state = {}) {
    const root = mkdtempSync(join(tmpdir(), 'simplex-docker-preflight-'));
    const calls = join(root, 'calls.jsonl');
    const executable = join(root, 'docker');
    const description = { status: 'running', running: true, networkMode: 'host', extraHosts: [], ...state };
    writeFileSync(executable, `#!${process.execPath}
import { appendFileSync } from 'node:fs';
import { spawn } from 'node:child_process';
const args = process.argv.slice(2);
appendFileSync(process.env.CALLS, JSON.stringify(args) + '\\n');
const state = JSON.parse(process.env.STATE);
if (args[0] === 'inspect') {
    if (state.inspect === 'missing') { console.error('Error: No such object: ' + args.at(-1)); process.exit(1); }
    if (state.inspect === 'daemon') { console.error('permission denied while connecting to the Docker daemon'); process.exit(1); }
    if (state.inspect === 'hang') { setInterval(() => {}, 1000); }
    else {
        const hosts = JSON.stringify(state.extraHosts ?? []);
        process.stdout.write([state.status, String(state.running), state.networkMode, hosts].join('\\t') + '\\n');
        process.exit(0);
    }
} else if (args[0] === 'exec') {
    if (state.exec === 'missing') {
        console.error('OCI runtime exec failed: exec: "simplex": executable file not found in $PATH');
        process.exit(127);
    }
    if (state.exec === 'failed') { console.error('worker refused to start'); process.exit(2); }
    if (state.exec === 'hang') { setInterval(() => {}, 1000); }
    else if (args[3] === '--version' || !state.delegate) { process.stdout.write('simplex 1.1.2\\n'); process.exit(0); }
    else {
        const child = spawn(args[2], args.slice(3), { stdio: 'inherit' });
        child.on('exit', code => { process.exitCode = code ?? 1; });
    }
} else {
    console.error('unexpected Docker command: ' + args[0]);
    process.exit(99);
}
`, { mode: 0o700 });
    t.after(() => rmSync(root, { recursive: true, force: true }));
    return {
        root,
        executable,
        env: { CALLS: calls, STATE: JSON.stringify(description) },
        calls() {
            return existsSync(calls)
                ? readFileSync(calls, 'utf8').trim().split('\n').filter(Boolean).map(line => JSON.parse(line))
                : [];
        },
    };
}

function targetFor(fixture, preflight) {
    return {
        kind: 'docker', command: fixture.executable, args: [], container: 'retained',
        ...(preflight === undefined ? {} : { preflight }),
    };
}

async function check(fixture, { preflight, endpointsOverride = endpoints, executable = 'simplex' } = {}) {
    return preflightDockerTarget({
        target: targetFor(fixture, preflight),
        executable,
        cwd: fixture.root,
        env: fixture.env,
        endpoints: endpointsOverride,
    });
}

function codeOf(error) {
    assert.ok(error instanceof DockerPreflightError, String(error));
    return error.code;
}

it('runs the container and installation checks in order without a lifetime command', async t => {
    const fixture = dockerFixture(t);
    await check(fixture);
    const calls = fixture.calls();
    assert.deepEqual(calls.map(call => call[0]), ['inspect', 'exec']);
    assert.deepEqual(calls[1].slice(0, 4), ['exec', 'retained', 'simplex', '--version']);
    assert.ok(!calls.some(call => ['run', 'create', 'start', 'stop', 'rm', 'kill'].includes(call[0])));
});

it('reports a missing container separately from a stopped one', async t => {
    const missing = dockerFixture(t, { inspect: 'missing' });
    await assert.rejects(check(missing), error => {
        assert.equal(codeOf(error), 'container-missing');
        assert.match(error.message, /container "retained" does not exist/);
        return true;
    });
    const stopped = dockerFixture(t, { status: 'exited', running: false });
    await assert.rejects(check(stopped), error => {
        assert.equal(codeOf(error), 'container-not-running');
        assert.match(error.message, /status: exited/);
        return true;
    });
    assert.deepEqual(stopped.calls().map(call => call[0]), ['inspect']);
});

it('keeps a sanitized Docker CLI diagnostic without exposing the environment', async t => {
    const fixture = dockerFixture(t, { inspect: 'daemon' });
    await assert.rejects(check(fixture), error => {
        assert.equal(codeOf(error), 'inspect-failed');
        assert.match(error.message, /permission denied/);
        assert.ok(!error.message.includes('CALLS'));
        return true;
    });
});

it('refuses a container whose installation cannot answer --version', async t => {
    const missing = dockerFixture(t, { exec: 'missing' });
    await assert.rejects(check(missing), error => {
        assert.equal(codeOf(error), 'executable-missing');
        assert.match(error.message, /exit 127/);
        return true;
    });
    const failed = dockerFixture(t, { exec: 'failed' });
    await assert.rejects(check(failed), error => {
        assert.equal(codeOf(error), 'executable-failed');
        assert.match(error.message, /exit 2/);
        return true;
    });
});

it('rejects loopback endpoints unless the container shares the host network', async t => {
    const bridged = dockerFixture(t, { networkMode: 'bridge' });
    await assert.rejects(check(bridged), error => {
        assert.equal(codeOf(error), 'endpoint-loopback');
        assert.match(error.message, /127\.0\.0\.1:8800/);
        assert.ok(!error.message.includes('secret-token'), 'the session token must not reach an error message');
        return true;
    });
    const hostNetwork = dockerFixture(t, { networkMode: 'host' });
    await check(hostNetwork);
    const none = dockerFixture(t, { networkMode: 'none' });
    await assert.rejects(check(none), error => {
        assert.equal(codeOf(error), 'endpoint-network-none');
        return true;
    });
});

it('requires an explicit host mapping for host.docker.internal', async t => {
    const internal = {
        ...endpoints,
        events: 'ws://host.docker.internal:8800/worker/events?token=secret-token',
    };
    const unmapped = dockerFixture(t, { networkMode: 'bridge', extraHosts: [] });
    await assert.rejects(check(unmapped, { endpointsOverride: internal }), error => {
        assert.equal(codeOf(error), 'endpoint-host-mapping');
        assert.match(error.message, /--add-host=host\.docker\.internal:host-gateway/);
        return true;
    });
    const mapped = dockerFixture(t, {
        networkMode: 'bridge', extraHosts: ['host.docker.internal:host-gateway'],
    });
    await check(mapped, { endpointsOverride: internal });
});

it('honors the per-check switches and the overall disable switch', async t => {
    const containerOnly = dockerFixture(t);
    await check(containerOnly, { preflight: { executable: false, endpoint: { enabled: false } } });
    assert.deepEqual(containerOnly.calls().map(call => call[0]), ['inspect']);

    const disabled = dockerFixture(t);
    await check(disabled, { preflight: { enabled: false } });
    assert.deepEqual(disabled.calls(), []);
});

it('reports an unresponsive Docker CLI or an unusable command', async t => {
    const hanging = dockerFixture(t, { inspect: 'hang' });
    await assert.rejects(check(hanging, { preflight: { timeoutMs: 100 } }), error => {
        assert.equal(codeOf(error), 'inspect-timeout');
        assert.match(error.message, /100 ms/);
        return true;
    });
    const unavailable = dockerFixture(t);
    await assert.rejects(preflightDockerTarget({
        target: { kind: 'docker', command: join(unavailable.root, 'absent'), args: [], container: 'retained' },
        executable: 'simplex', cwd: unavailable.root, env: {}, endpoints,
    }), error => {
        assert.equal(codeOf(error), 'docker-unavailable');
        return true;
    });
});

it('normalizes omitted checks and rejects unusable policies', () => {
    assert.deepEqual(normalizeDockerPreflight(undefined), defaults);
    assert.deepEqual(normalizeDockerPreflight({ endpoint: { channels: ['tools'] } }), {
        ...defaults, endpoint: { enabled: true, channels: ['tools'] },
    });
    for (const invalid of [
        null, [], 'yes', { timeoutMs: 0 }, { timeoutMs: 1.5 }, { enabled: 'yes' },
        { endpoint: { channels: [] } }, { endpoint: { channels: ['events', 'events'] } },
        { endpoint: { channels: ['unknown'] } }, { unknown: true }, { endpoint: { enabled: 'yes' } },
    ]) {
        assert.throws(() => normalizeDockerPreflight(invalid), /docker preflight/);
    }
    const base = { executable: 'simplex', cwd: tmpdir(), env: {}, root: '/target/allocations' };
    assert.throws(() => managementPolicy({ ...base, target: { kind: 'local', preflight: {} } }),
        /preflight is only valid/);
});

it('refuses to bind an allocation when the container is missing', async t => {
    const fixture = dockerFixture(t, { inspect: 'missing' });
    const ctx = await startTestHub({});
    t.after(() => ctx.hub.stop());
    const store = new ConfigurationStore(ctx.config);
    store.save('launch', 'docker-preflight', JSON.stringify({
        management: {
            target: { kind: 'docker', command: fixture.executable, args: [], container: 'retained' },
            executable: 'simplex', cwd: fixture.root, env: fixture.env, root: '/only-on-target/allocations',
        },
    }), null);
    snapshotConfigs(store, 'preflight-failure', { launchConfig: 'docker-preflight', workerConfig: 'default' });
    const session = ctx.hub.registry.create('preflight-failure');
    const result = await ctx.hub.supervisor.start(session);
    assert.equal(result.ok, false);
    assert.match(result.error, /^docker preflight failed: container "retained" does not exist$/);
    assert.equal(readManagedSession(ctx.config, 'preflight-failure'), null,
        'a failed preflight must leave the session unbound and editable');
    assert.deepEqual(fixture.calls().map(call => call[0]), ['inspect']);
});

it('dispatches a managed worker only after the preflight passes', async t => {
    const fixture = dockerFixture(t, { networkMode: 'host', delegate: true });
    const managedRoot = join(fixture.root, 'target');
    mkdirSync(managedRoot);
    const executable = join(fixture.root, 'simplex.mjs');
    writeFileSync(executable, `#!${process.execPath}\nimport ${JSON.stringify(new URL('./fixtures/managed-target.mjs', import.meta.url).href)};\n`, { mode: 0o700 });
    const ctx = await startTestHub({ worker: { stopTimeoutMs: 200, sigtermGraceMs: 1000, sigkillGraceMs: 1000 } });
    let dispatchedPid = null;
    t.after(async () => {
        // The fixture worker is not a protocol peer, so an unconfirmed remote
        // stop cannot end it. Release the pid observed during the test; the
        // fixture root may already be gone when later cleanup hooks run.
        if (dispatchedPid) try { process.kill(dispatchedPid, 'SIGKILL'); } catch {}
        await ctx.hub.stop();
    });
    const store = new ConfigurationStore(ctx.config);
    store.save('launch', 'docker-managed', JSON.stringify({
        management: {
            target: { kind: 'docker', command: fixture.executable, args: [], container: 'retained' },
            executable,
            cwd: fixture.root,
            env: { ...fixture.env, MANAGED_FIXTURE_ROOT: managedRoot },
            root: '/only-on-target/allocations',
        },
        worker: { threads: 1, stopTimeoutMs: 200, sigtermGraceMs: 200, sigkillGraceMs: 200 },
    }), null);
    snapshotConfigs(store, 'managed-docker', { launchConfig: 'docker-managed', workerConfig: 'default' });
    const session = ctx.hub.registry.create('managed-docker');
    const result = await ctx.hub.supervisor.start(session);
    assert.equal(result.ok, true, result.error);
    const end = Date.now() + 4000;
    while (!existsSync(join(managedRoot, 'last-run.json'))) {
        assert.ok(Date.now() < end, 'the fixture worker never received its run command');
        await new Promise(resolve => setTimeout(resolve, 10));
    }
    dispatchedPid = Number(readFileSync(join(managedRoot, 'dispatched.pid'), 'utf8'));
    const calls = fixture.calls();
    assert.deepEqual(calls[0], ['inspect', '--format',
        '{{.State.Status}}\t{{.State.Running}}\t{{.HostConfig.NetworkMode}}\t{{json .HostConfig.ExtraHosts}}',
        'retained']);
    assert.deepEqual(calls[1].slice(0, 4), ['exec', 'retained', executable, '--version']);
    assert.ok(!calls.some(call => ['run', 'create', 'start', 'stop', 'rm', 'kill'].includes(call[0])));
    const record = readManagedSession(ctx.config, 'managed-docker');
    assert.equal(record.reference.directory, '/only-on-target/allocations/' + record.reference.allocationId);
});
