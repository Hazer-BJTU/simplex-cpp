/** Docker startup checks: read-only, ordered, and never a container lifetime command. */
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { it } from 'node:test';
import { DockerPreflightError, DOCKER_PREFLIGHT_CLEANUP_MS, preflightDockerTarget } from '../src/launch/docker-preflight.ts';
import { normalizeDockerPreflight } from '../src/launch/managed-launcher.ts';
import { managementPolicy, readManagedSession } from '../src/launch/managed-sessions.ts';
import { ConfigurationStore } from '../src/configurations/store.ts';
import { snapshotConfigs } from '../src/configurations/session.ts';
import { createHub } from '../src/hub.ts';
import { hubRoot } from '../src/config.ts';
import { createLogger } from '../src/log.ts';
import { startTestHub, testConfig } from './helpers/hub.js';

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
    if (state.inspect === 'credentials') {
        console.error('cannot connect https://operator:FAKE_AUDIT_SECRET@daemon.invalid/?token=QUERY_TOKEN');
        process.exit(1);
    }
    if (state.inspect === 'environment') {
        console.error('daemon rejected environment ' + process.env.AUDIT_SECRET);
        process.exit(1);
    }
    if (state.inspect === 'noisy') {
        console.error('X'.repeat(20000));
        process.exit(1);
    }
    if (state.inspect === 'hang' || state.inspect === 'hang-secret') {
        if (state.inspect === 'hang-secret') console.error('inspect failed for FAKE_AUDIT_SECRET');
        setInterval(() => {}, 1000);
    } else {
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
    if (state.exec === 'secret-missing') { console.error('exec failed for FAKE_AUDIT_SECRET'); process.exit(127); }
    if (state.exec === 'secret') { console.error('exec failed for FAKE_AUDIT_SECRET'); process.exit(2); }
    if (state.exec === 'hang' || state.exec === 'secret-hang') {
        if (state.exec === 'secret-hang') console.error('exec failed for FAKE_AUDIT_SECRET');
        setInterval(() => {}, 1000);
    } else if (args[3] === '--version' || !state.delegate) { process.stdout.write('simplex 1.1.2\\n'); process.exit(0); }
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
        env: { CALLS: calls, STATE: JSON.stringify(description), AUDIT_SECRET: 'FAKE_ENV_SECRET' },
        calls() {
            return existsSync(calls)
                ? readFileSync(calls, 'utf8').trim().split('\n').filter(Boolean).map(line => JSON.parse(line))
                : [];
        },
    };
}

/**
 * A Docker CLI whose inspect answer is immediate and whose exec never returns.
 * The shell keeps the inspect phase far below the deadline, so the timeout under
 * test is the executable probe rather than Node startup.
 */
function hangingExecFixture(t) {
    const root = mkdtempSync(join(tmpdir(), 'simplex-docker-preflight-'));
    const executable = join(root, 'docker');
    writeFileSync(executable, `#!/bin/sh
if [ "$1" = "inspect" ]; then
    printf 'running\\ttrue\\thost\\t[]\\n'
    exit 0
fi
exec sleep 1000
`, { mode: 0o700 });
    t.after(() => rmSync(root, { recursive: true, force: true }));
    return { root, executable, env: {} };
}

/**
 * A wrapper that leaves a descendant holding both inherited pipes. The direct
 * process dies at the deadline, but `close` cannot arrive until the background
 * sleep ends; only the cleanup allowance can settle the call.
 */
function pipeHoldingFixture(t) {
    const root = mkdtempSync(join(tmpdir(), 'simplex-docker-preflight-'));
    const executable = join(root, 'docker');
    writeFileSync(executable, `#!/bin/sh
sleep 5 &
wait
`, { mode: 0o700 });
    t.after(() => rmSync(root, { recursive: true, force: true }));
    return { root, executable, env: {} };
}

/**
 * A wrapper that answers both probes and exits 0 while a helper inherits the
 * pipes. `close` never arrives; only the recorded exit status can prove success.
 */
function answeringWrapperFixture(t) {
    const root = mkdtempSync(join(tmpdir(), 'simplex-docker-preflight-'));
    const executable = join(root, 'docker');
    writeFileSync(executable, `#!/bin/sh
if [ "$1" = "inspect" ]; then
    printf 'running\\ttrue\\thost\\t[]\\n'
else
    printf 'simplex test\\n'
fi
sleep 5 &
exit 0
`, { mode: 0o700 });
    t.after(() => rmSync(root, { recursive: true, force: true }));
    return { root, executable, env: {} };
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

/** Persist one Docker launch configuration and the snapshot a start reads. */
function saveLaunch(ctx, name, fixture, preflight) {
    const store = new ConfigurationStore(ctx.config);
    store.save('launch', name, JSON.stringify({
        management: {
            target: { kind: 'docker', command: fixture.executable, args: [], container: 'retained',
                ...(preflight === undefined ? {} : { preflight }) },
            executable: 'simplex', cwd: fixture.root, env: fixture.env,
            root: '/only-on-target/allocations',
        },
    }), null);
    snapshotConfigs(store, name, { launchConfig: name, workerConfig: 'default' });
}

/** Start a hub whose log lines are captured at one level. */
async function startLoggedHub(t, level) {
    const config = testConfig({});
    const lines = [];
    const hub = createHub({
        config, hubRoot, version: 'test',
        log: createLogger({ level, sink: line => lines.push(line) }),
    });
    await hub.start();
    t.after(() => hub.stop());
    return { config, hub, lines };
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

it('keeps CLI diagnostics private and public text controlled', async t => {
    const daemon = dockerFixture(t, { inspect: 'daemon' });
    await assert.rejects(check(daemon), error => {
        assert.equal(codeOf(error), 'inspect-failed');
        assert.match(error.message, /cannot inspect container "retained" \(exit 1\)/);
        assert.match(error.message, /check Docker CLI permissions/,
            'a controlled hint keeps a permission failure actionable');
        assert.ok(!error.message.includes('permission denied'),
            'CLI stderr must not enter the public message');
        assert.match(error.detail ?? '', /permission denied/);
        return true;
    });
    const credentials = dockerFixture(t, { inspect: 'credentials' });
    await assert.rejects(check(credentials), error => {
        assert.equal(codeOf(error), 'inspect-failed');
        assert.match(error.message, /check that the Docker daemon is reachable/);
        for (const secret of ['FAKE_AUDIT_SECRET', 'QUERY_TOKEN']) {
            assert.ok(!error.message.includes(secret), `${secret} must not reach the public message`);
            assert.ok(error.detail?.includes(secret), `${secret} stays available privately`);
        }
        return true;
    });
    const environment = dockerFixture(t, { inspect: 'environment' });
    await assert.rejects(check(environment), error => {
        assert.equal(codeOf(error), 'inspect-failed');
        assert.ok(!error.message.includes('FAKE_ENV_SECRET'));
        assert.ok(error.detail?.includes('FAKE_ENV_SECRET'));
        return true;
    });
    const noisy = dockerFixture(t, { inspect: 'noisy' });
    await assert.rejects(check(noisy), error => {
        assert.equal(codeOf(error), 'inspect-failed');
        assert.equal(error.detail?.length, 200, 'the private diagnostic stays bounded');
        assert.ok(!error.message.includes('XXXXXXXXXX'));
        return true;
    });
});

it('keeps CLI text private for probe failures and timeouts', async t => {
    const cases = [
        [{ inspect: 'hang-secret' }, 'inspect-timeout'],
        [{ exec: 'secret-missing' }, 'executable-missing'],
        [{ exec: 'secret' }, 'executable-failed'],
        [{ exec: 'secret-hang' }, 'executable-timeout'],
    ];
    for (const [state, code] of cases) {
        const fixture = dockerFixture(t, state);
        await assert.rejects(check(fixture, { preflight: { timeoutMs: 1000 } }), error => {
            assert.equal(codeOf(error), code);
            assert.ok(!error.message.includes('FAKE_AUDIT_SECRET'),
                `${code} must not forward CLI text`);
            assert.ok(error.detail?.includes('FAKE_AUDIT_SECRET'),
                `${code} keeps the raw line privately`);
            return true;
        });
    }
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

it('echoes only Docker vocabulary from inspect labels', async t => {
    const tokenStatus = dockerFixture(t, { status: 'FAKE_AUDIT_SECRET', running: false });
    await assert.rejects(check(tokenStatus, { preflight: { executable: false } }), error => {
        assert.equal(codeOf(error), 'container-not-running');
        assert.match(error.message, /status: unknown/);
        assert.ok(!error.message.includes('FAKE_AUDIT_SECRET'));
        return true;
    });
    const paused = dockerFixture(t, { status: 'paused', running: false });
    await assert.rejects(check(paused, { preflight: { executable: false } }), error => {
        assert.match(error.message, /status: paused/);
        return true;
    });
    const tokenNetwork = dockerFixture(t, { networkMode: 'FAKE_AUDIT_SECRET' });
    await assert.rejects(check(tokenNetwork), error => {
        assert.equal(codeOf(error), 'endpoint-loopback');
        assert.match(error.message, /network mode "user-defined"/);
        assert.ok(!error.message.includes('FAKE_AUDIT_SECRET'));
        return true;
    });
    const bridged = dockerFixture(t, { networkMode: 'bridge' });
    await assert.rejects(check(bridged), error => {
        assert.match(error.message, /network mode "bridge"/);
        return true;
    });
    // A shared network namespace is accepted; its reference stays diagnosable.
    const shared = dockerFixture(t, { networkMode: 'container:abc123' });
    await check(shared);
});

it('honors the per-check switches and the overall disable switch', async t => {
    const containerOnly = dockerFixture(t);
    await check(containerOnly, { preflight: { executable: false, endpoint: { enabled: false } } });
    assert.deepEqual(containerOnly.calls().map(call => call[0]), ['inspect']);

    const disabled = dockerFixture(t);
    await check(disabled, { preflight: { enabled: false } });
    assert.deepEqual(disabled.calls(), []);
});

it('always requires existence and reports a stopped container when the probe is enabled', async t => {
    const stopped = dockerFixture(t, { status: 'exited', running: false });
    await assert.rejects(check(stopped, { preflight: { container: false, endpoint: { enabled: false } } }), error => {
        assert.equal(codeOf(error), 'container-not-running');
        return true;
    });
    const missing = dockerFixture(t, { inspect: 'missing' });
    await assert.rejects(check(missing, { preflight: { container: false, endpoint: { enabled: false } } }), error => {
        assert.equal(codeOf(error), 'container-missing');
        return true;
    });
});

it('reports an unresponsive Docker CLI or an unusable command', async t => {
    const hanging = dockerFixture(t, { inspect: 'hang' });
    let started = Date.now();
    await assert.rejects(check(hanging, { preflight: { timeoutMs: 100 } }), error => {
        assert.equal(codeOf(error), 'inspect-timeout');
        assert.match(error.message, /100 ms/);
        return true;
    });
    assert.ok(Date.now() - started < 100 + DOCKER_PREFLIGHT_CLEANUP_MS + 1000,
        'a timed-out inspect must settle within its deadline and cleanup allowance');
    const unavailable = dockerFixture(t);
    await assert.rejects(preflightDockerTarget({
        target: { kind: 'docker', command: join(unavailable.root, 'absent'), args: [], container: 'retained' },
        executable: 'simplex', cwd: unavailable.root, env: {}, endpoints,
    }), error => {
        assert.equal(codeOf(error), 'docker-unavailable');
        return true;
    });
    // Inspect answers immediately here, so the deadline under test cannot be
    // consumed by Node startup before the hanging executable probe runs.
    const hangingExec = hangingExecFixture(t);
    started = Date.now();
    await assert.rejects(check(hangingExec, { preflight: { timeoutMs: 100 } }), error => {
        assert.equal(codeOf(error), 'executable-timeout');
        assert.match(error.message, /100 ms/);
        return true;
    });
    assert.ok(Date.now() - started < 100 + DOCKER_PREFLIGHT_CLEANUP_MS + 1000,
        'a timed-out executable probe must settle within its deadline and cleanup allowance');
});

it('settles a timed-out command when a descendant keeps its pipes open', async t => {
    const fixture = pipeHoldingFixture(t);
    const started = Date.now();
    await assert.rejects(check(fixture, { preflight: { timeoutMs: 80 } }), error => {
        assert.equal(codeOf(error), 'inspect-timeout');
        return true;
    });
    const elapsed = Date.now() - started;
    assert.ok(elapsed < 80 + DOCKER_PREFLIGHT_CLEANUP_MS + 1200,
        `preflight settled after ${elapsed} ms; it must not wait for the inherited pipes`);
});

it('keeps the real status when a command exits before the deadline', async t => {
    // The wrapper answers both probes and exits 0; the helper it leaves behind
    // keeps `close` away. The recorded exit status must win over the deadline.
    const fixture = answeringWrapperFixture(t);
    const started = Date.now();
    await check(fixture, { preflight: { timeoutMs: 200 } });
    assert.ok(Date.now() - started < (200 + DOCKER_PREFLIGHT_CLEANUP_MS) * 2 + 1500,
        'a command that exited before its deadline must not be reported as a timeout');
});

it('lets the process exit after settlement when a descendant holds the pipes', async t => {
    // Without releasing the pipe read ends, the event loop would stay alive for
    // as long as the wrapper's descendant holds the write ends (5 s here).
    const fixture = pipeHoldingFixture(t);
    const moduleUrl = new URL('../src/launch/docker-preflight.ts', import.meta.url).href;
    const script = `
import { preflightDockerTarget } from ${JSON.stringify(moduleUrl)};
const target = { kind: 'docker', command: ${JSON.stringify(fixture.executable)}, args: [],
    container: 'retained', preflight: { timeoutMs: 50 } };
try {
    await preflightDockerTarget({
        target, executable: 'simplex', cwd: ${JSON.stringify(fixture.root)}, env: {},
        endpoints: {
            events: 'ws://127.0.0.1:1/events',
            confirm: 'ws://127.0.0.1:1/confirm',
            tools: 'ws://127.0.0.1:1/tools',
        },
    });
    process.stdout.write('resolved\\n');
} catch (error) {
    process.stdout.write('settled:' + error.code + '\\n');
}
`;
    const started = Date.now();
    const result = await new Promise((resolve, reject) => {
        const child = spawn(process.execPath, ['--input-type=module', '-e', script], {
            cwd: fixture.root, stdio: ['ignore', 'pipe', 'ignore'],
        });
        let output = '';
        child.stdout.on('data', chunk => { output += chunk; });
        const timer = setTimeout(() => {
            child.kill('SIGKILL');
            reject(new Error('the preflight process stayed alive on inherited pipes'));
        }, 3000);
        child.once('exit', code => {
            clearTimeout(timer);
            resolve({ code, output, elapsed: Date.now() - started });
        });
    });
    assert.equal(result.code, 0);
    assert.match(result.output, /settled:inspect-timeout/);
    assert.ok(result.elapsed < 2500, `the process exited after ${result.elapsed} ms`);
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

it('bounds startup ownership when a preflight deadline expires', async t => {
    const fixture = pipeHoldingFixture(t);
    const ctx = await startTestHub({});
    t.after(() => ctx.hub.stop());
    saveLaunch(ctx, 'docker-preflight-timeout', fixture, { timeoutMs: 50 });
    const session = ctx.hub.registry.create('docker-preflight-timeout');
    const bound = 50 + DOCKER_PREFLIGHT_CLEANUP_MS + 1200;
    let started = Date.now();
    const first = await ctx.hub.supervisor.start(session);
    assert.equal(first.ok, false);
    assert.match(first.error, /^docker preflight failed: Docker did not answer within 50 ms/);
    assert.ok(Date.now() - started < bound,
        'a timed-out preflight must not keep the startup reservation');
    // Ownership must be released: the same session can be tried again and
    // fails on its own deadline instead of waiting on the previous start.
    started = Date.now();
    const second = await ctx.hub.supervisor.start(session);
    assert.equal(second.ok, false);
    assert.ok(Date.now() - started < bound,
        'a repeated start must not wait on the previous timeout');
    assert.equal(readManagedSession(ctx.config, 'docker-preflight-timeout'), null,
        'a timed-out preflight must leave the session unbound');
});

it('shutdown does not wait for a preflight that exceeded its deadline', async t => {
    const fixture = pipeHoldingFixture(t);
    const ctx = await startTestHub({});
    let stopped = false;
    t.after(async () => { if (!stopped) await ctx.hub.stop(); });
    saveLaunch(ctx, 'docker-preflight-shutdown', fixture, { timeoutMs: 50 });
    const session = ctx.hub.registry.create('docker-preflight-shutdown');
    const pending = ctx.hub.supervisor.start(session);
    // Let the start reach the preflight before shutdown takes its inventory.
    await new Promise(resolve => setTimeout(resolve, 20));
    const started = Date.now();
    await ctx.hub.stop();
    stopped = true;
    const result = await pending;
    assert.equal(result.ok, false);
    assert.ok(Date.now() - started < 50 + DOCKER_PREFLIGHT_CLEANUP_MS + 1200,
        'shutdown must not wait for a pipe held by the Docker CLI');
});

it('keeps private CLI text out of public errors and default logs', async t => {
    const fixture = dockerFixture(t, { inspect: 'credentials' });
    const ctx = await startLoggedHub(t, 'info');
    saveLaunch(ctx, 'docker-preflight-log', fixture);
    const session = ctx.hub.registry.create('docker-preflight-log');
    const result = await ctx.hub.supervisor.start(session);
    assert.equal(result.ok, false);
    assert.match(result.error, /cannot inspect container "retained" \(exit 1\)/);
    const log = ctx.lines.join('\n');
    assert.match(log, /docker preflight failed: cannot inspect container "retained" \(exit 1\)/);
    for (const secret of ['FAKE_AUDIT_SECRET', 'QUERY_TOKEN']) {
        assert.ok(!result.error.includes(secret), `${secret} must not reach the public result`);
        assert.ok(!log.includes(secret), `${secret} must not reach the default log`);
    }

    // Deliberate debug logging is the only retained copy of the raw line; the
    // public result still excludes it.
    const debugCtx = await startLoggedHub(t, 'debug');
    saveLaunch(debugCtx, 'docker-preflight-debug', fixture);
    const debugResult = await debugCtx.hub.supervisor.start(
        debugCtx.hub.registry.create('docker-preflight-debug'));
    assert.equal(debugResult.ok, false);
    assert.match(debugCtx.lines.join('\n'),
        /docker preflight diagnostic: cannot connect https:\/\/operator:FAKE_AUDIT_SECRET@daemon\.invalid\//);
    assert.ok(!debugResult.error.includes('FAKE_AUDIT_SECRET'));
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
    const dispatched = join(managedRoot, 'dispatched.pid');
    while (!existsSync(join(managedRoot, 'last-run.json'))) {
        // Capture the pid as soon as it appears: a later assertion may throw,
        // and the teardown hook must still be able to end this fixture worker.
        if (!dispatchedPid && existsSync(dispatched)) {
            dispatchedPid = Number(readFileSync(dispatched, 'utf8'));
        }
        assert.ok(Date.now() < end, 'the fixture worker never received its run command');
        await new Promise(resolve => setTimeout(resolve, 10));
    }
    if (!dispatchedPid && existsSync(dispatched)) dispatchedPid = Number(readFileSync(dispatched, 'utf8'));
    const calls = fixture.calls();
    assert.deepEqual(calls[0], ['inspect', '--format',
        '{{.State.Status}}\t{{.State.Running}}\t{{.HostConfig.NetworkMode}}\t{{json .HostConfig.ExtraHosts}}',
        'retained']);
    assert.deepEqual(calls[1].slice(0, 4), ['exec', 'retained', executable, '--version']);
    assert.ok(!calls.some(call => ['run', 'create', 'start', 'stop', 'rm', 'kill'].includes(call[0])));
    const record = readManagedSession(ctx.config, 'managed-docker');
    assert.equal(record.reference.directory, '/only-on-target/allocations/' + record.reference.allocationId);
});
