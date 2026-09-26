/**
 * @file P0 hardening regressions.
 *
 * Every test here pins one defect that could end the hub process, answer a
 * client with an opaque 500, or signal an unrelated process. They live in one
 * file on purpose: the shared property is "the hub survives an input it does
 * not control", and a reader should be able to see the whole set at once.
 *
 * The three crash paths were reachable from the network before this set
 * existed:
 *   - a malformed `Host` header threw out of the request handler,
 *   - a failing worker action rejected out of the panel socket handler,
 *   - an unopenable worker log file emitted an unhandled stream 'error'.
 *
 * None of them needs a credential, and the hub treats all three as fatal.
 */
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdirSync, mkdtempSync, writeFileSync } from 'node:fs';
import { connect } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, describe, it } from 'node:test';
import { ProcessRecord, PROCESS_STATE, readProcessStartTime } from '../src/launch/supervisor.ts';
import { sessionDir } from '../src/launch/config-render.ts';
import { resolveStaticPath } from '../src/http/static.ts';
import { RingBuffer } from '../src/util/ring.ts';
import { ConfigError, hubRoot, loadConfig } from '../src/config.ts';
import { startTestHub } from './helpers/hub.js';
import { connectWorker, until, workerEvent } from './helpers/worker.js';

/** Hub configuration that spawns the fixture instead of the C++ worker. */
function supervisorOverrides(extra = {}) {
    return {
        worker: {
            bin: join(import.meta.dirname, 'fixtures', 'fake-worker.js'),
            stopTimeoutMs: 100,
            sigtermGraceMs: 200,
            sigkillGraceMs: 1000,
            ...extra,
        },
    };
}

/**
 * Write raw bytes to a TCP port and read the response head.
 *
 * `fetch` normalises the `Host` header, so the only way to present the hub with
 * an authority Node will not parse is to speak HTTP by hand.
 */
function rawExchange(port, payload, { timeoutMs = 5000 } = {}) {
    return new Promise((resolve, reject) => {
        const socket = connect(port, '127.0.0.1');
        let received = '';
        const finish = () => {
            clearTimeout(timer);
            socket.destroy();
            resolve(received);
        };
        const timer = setTimeout(finish, timeoutMs);
        socket.setEncoding('utf8');
        socket.on('connect', () => socket.write(payload));
        socket.on('data', (chunk) => {
            received += chunk;
            if (received.includes('\r\n\r\n')) finish();
        });
        socket.on('error', (error) => {
            clearTimeout(timer);
            // A connection reset after a response head is still a response.
            if (received.length > 0) resolve(received);
            else reject(error);
        });
    });
}

/** A ProcessRecord with no process behind it, for target-resolution tests. */
function bareRecord({ sessionId = 'probe', pid = null, pidStartTime = null, pidFile = null } = {}) {
    const record = new ProcessRecord({
        sessionId,
        invocation: { command: 'probe', args: [], cwd: '', pidFile },
        logPath: null,
        logStream: { write() {}, end() {} },
        logs: new RingBuffer({ limit: 10, byteLimit: 1024 }),
    });
    record.pid = pid;
    record.pidStartTime = pidStartTime;
    return record;
}

/** Sleep, for the one case with no observable event to await. */
const delay = (ms) => new Promise((resolve) => { setTimeout(resolve, ms); });

describe('P0 hardening', () => {
    const hubs = [];

    /** Start a hub and remember it for cleanup. */
    async function setup(overrides = supervisorOverrides(), hooks = {}) {
        const ctx = await startTestHub(overrides, hooks);
        hubs.push(ctx);
        return ctx;
    }

    after(async () => {
        for (const ctx of hubs) {
            // A hub whose dataDir is deliberately unusable may not flush cleanly.
            await ctx.hub.stop().catch(() => {});
        }
    });

    describe('a malformed request authority is answered, not fatal', () => {
        it('answers a bad Host header with 400 and keeps serving', async () => {
            const ctx = await setup();
            const response = await rawExchange(ctx.port,
                'GET /api/meta HTTP/1.1\r\nHost: foo bar\r\nConnection: close\r\n\r\n');
            assert.match(response, /^HTTP\/1\.1 400 /, response.split('\r\n')[0]);

            const after = await fetch(`${ctx.base}/api/meta`);
            assert.equal(after.status, 200, 'the hub stopped serving after a bad Host header');
        });

        it('answers a bad Host header on an upgrade with 400 and keeps serving', async () => {
            const ctx = await setup();
            const response = await rawExchange(ctx.port,
                'GET /panel/ws HTTP/1.1\r\nHost: foo bar\r\nUpgrade: websocket\r\n'
                + 'Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n'
                + 'Sec-WebSocket-Version: 13\r\n\r\n');
            assert.match(response, /^HTTP\/1\.1 400 /, response.split('\r\n')[0]);

            const after = await fetch(`${ctx.base}/api/meta`);
            assert.equal(after.status, 200, 'the hub stopped serving after a bad upgrade authority');
        });

        it('still accepts an ordinary request', async () => {
            const ctx = await setup();
            const response = await fetch(`${ctx.base}/api/meta`);
            assert.equal(response.status, 200);
        });
    });

    describe('a malformed percent-escape is a client error', () => {
        it('answers a bad escape in an API path with 400, not 500', async () => {
            const ctx = await setup();
            const response = await fetch(`${ctx.base}/api/sessions/%ZZ`);
            assert.equal(response.status, 400);
            const body = await response.json();
            assert.equal(body.error, 'bad_request');
            assert.doesNotMatch(body.message ?? '', /URIError/, 'the raw URIError text leaked');
        });

        it('answers a bad escape in a static path with 400, not 500', async () => {
            const ctx = await setup();
            const response = await fetch(`${ctx.base}/%ZZ`);
            assert.equal(response.status, 400);
        });

        it('refuses to resolve a path it cannot decode', () => {
            assert.equal(resolveStaticPath(join(hubRoot, 'web'), '/%ZZ'), null);
        });
    });

    describe('a failing worker action is answered, not thrown', () => {
        it('reports a directory it cannot create instead of rejecting', async () => {
            const ctx = await setup({ dataDir: join('/dev/null', 'nowhere') });
            ctx.hub.registry.create('nowhere');
            const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
            try {
                panel.send({ v: 1, type: 'worker', session: 'nowhere', action: 'start' });
                const reply = await panel.waitFor(
                    (message) => message.type === 'accepted' || message.type === 'error');
                assert.equal(reply.type, 'error');
                assert.equal(reply.error, 'worker_action_failed');
                assert.match(reply.message ?? '', /cannot create/);
                // The action name is echoed, which is what lets a panel report
                // which button failed.
                assert.equal(reply.action, 'start');

                const alive = await fetch(`${ctx.base}/api/meta`);
                assert.equal(alive.status, 200, 'the hub died on a failed worker action');
            } finally {
                await panel.close();
            }
        });
    });

    describe('an unusable worker log does not end the hub', () => {
        it('starts the worker and keeps serving when the log path is a directory', async () => {
            const ctx = await setup();
            const session = ctx.hub.registry.create('bad-log');
            // A directory where the log file belongs: `createWriteStream` fails
            // asynchronously, so only an 'error' listener keeps this contained.
            mkdirSync(join(sessionDir(ctx.config, session.id), 'worker.log'), { recursive: true });

            const started = await ctx.hub.supervisor.start(session);
            assert.equal(started.ok, true, started.error);
            assert.equal(session.process.state, PROCESS_STATE.running);

            // Give the stream's error event time to fire.
            await delay(200);
            const alive = await fetch(`${ctx.base}/api/meta`);
            assert.equal(alive.status, 200, 'the hub died on an unopenable log stream');
        });
    });

    describe('signals only target a verified process incarnation', () => {
        // These depend on `/proc` being readable, which is what makes pid reuse
        // detectable at all. The suite runs on Linux; on a platform without
        // `/proc` the supervisor deliberately falls back to trusting the pid,
        // and the assertion below fails loudly rather than passing vacuously.
        it('resolves the spawned pid while its start time still matches', async () => {
            const ctx = await setup();
            const startTime = readProcessStartTime(process.pid);
            assert.ok(startTime, 'this platform exposes no /proc start time');
            const record = bareRecord({ pid: process.pid, pidStartTime: startTime });
            assert.equal(ctx.hub.supervisor.targetPid(record), process.pid);
        });

        it('refuses the spawned pid once its recorded incarnation is gone', async () => {
            const ctx = await setup();
            // A start time of "0" is not this process's, which is exactly what a
            // reused pid looks like.
            const record = bareRecord({ pid: process.pid, pidStartTime: '0' });
            assert.equal(ctx.hub.supervisor.targetPid(record), null);
        });

        it('refuses a pid-file target whose incarnation changed', async () => {
            const ctx = await setup();
            const pidFile = join(ctx.config.dataDir, 'daemon.pid');
            writeFileSync(pidFile, `${process.pid}\n`);
            const record = bareRecord({ pid: 1, pidFile });

            // First sighting records which incarnation the pid file names.
            assert.equal(ctx.hub.supervisor.targetPid(record), process.pid);
            // A later reuse of that number must not be signalled.
            record.pidFileStartTime = '0';
            assert.equal(ctx.hub.supervisor.targetPid(record), null);
        });

        it('falls back to the spawned pid when there is no pid file', async () => {
            const ctx = await setup();
            const startTime = readProcessStartTime(process.pid);
            const record = bareRecord({ pid: process.pid, pidStartTime: startTime });
            assert.equal(ctx.hub.supervisor.targetPid(record), process.pid);
        });

        it('refuses to signal when there is no pid at all', async () => {
            const ctx = await setup();
            const record = bareRecord({ pid: null });
            assert.equal(ctx.hub.supervisor.targetPid(record), null);
            assert.equal(ctx.hub.supervisor.signalProcess(record, 'SIGTERM'), false);
        });
    });

    describe('configuration refuses keys it does not know', () => {
        it('names a misspelled nested key instead of ignoring it', () => {
            assert.throws(
                () => loadConfig({ overrides: { limits: { logLine: 10 } } }),
                (error) => error instanceof ConfigError
                    && error.message.includes('unknown configuration key: limits.logLine'));
        });

        it('names a misspelled top-level key', () => {
            assert.throws(
                () => loadConfig({ overrides: { dataDirectory: '/tmp/x' } }),
                (error) => error instanceof ConfigError
                    && error.message.includes('unknown configuration key: dataDirectory'));
        });

        it('reports every unknown key at once', () => {
            assert.throws(
                () => loadConfig({ overrides: { nope: 1, limits: { transcriptEvent: 1 } } }),
                (error) => /unknown configuration keys/.test(error.message)
                    && error.message.includes('nope')
                    && error.message.includes('limits.transcriptEvent'));
        });

        it('leaves the inside of a provider profile to the user', () => {
            // The hub forwards a profile to the worker largely as written, so its
            // keys are the plugin's vocabulary and must not be policed.
            const { config } = loadConfig({
                overrides: {
                    providerProfiles: {
                        mine: { plugin: 'deepseek', model: 'some-model', anything: { nested: true } },
                    },
                },
            });
            assert.deepEqual(config.providerProfiles.mine.anything, { nested: true });
        });

        it('still accepts the shipped example configuration', () => {
            const { config } = loadConfig({ file: join(hubRoot, 'hub.config.example.jsonc') });
            assert.ok(config.dataDir.length > 0);
        });
    });

    describe('a session spec is checked when the session is created', () => {
        const create = (ctx, body) => fetch(`${ctx.base}/api/sessions`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body),
        });

        it('refuses an unknown provider profile over REST', async () => {
            const ctx = await setup();
            const response = await create(ctx, { session: 'spec-a', spec: { provider: 'nope' } });
            assert.equal(response.status, 400);
            assert.match((await response.json()).message, /unknown provider profile/);
        });

        it('refuses a spec that is not an object', async () => {
            const ctx = await setup();
            const response = await create(ctx, { session: 'spec-b', spec: 'nope' });
            assert.equal(response.status, 400);
            assert.match((await response.json()).message, /spec must be a JSON object/);
        });

        it('refuses an impossible thread count over the panel socket', async () => {
            const ctx = await setup();
            const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
            try {
                panel.send({ v: 1, type: 'create_session', session: 'spec-c', spec: { threads: 0 } });
                const reply = await panel.waitFor(
                    (message) => message.type === 'created' || message.type === 'error');
                assert.equal(reply.type, 'error');
                assert.equal(reply.error, 'invalid_session');
                assert.match(reply.message, /spec\.threads/);
            } finally {
                await panel.close();
            }
        });

        it('still creates a session whose spec is usable', async () => {
            const ctx = await setup();
            const response = await create(ctx, {
                session: 'spec-ok', spec: { provider: 'mock', model: 'mock-auto' },
            });
            assert.equal(response.status, 201);
            assert.ok(ctx.hub.registry.get('spec-ok'));
        });
    });

    describe('a failing observer does not reach the socket callback', () => {
        // This is guarded twice on purpose: `WorkerConnection.emit` keeps a
        // throwing observer from reaching the socket's `message` listener, and
        // the hub contains each hook separately so one bad observer cannot
        // silence the others. The test asserts the behaviour both layers exist
        // to provide, so it fails only when both are gone — which is the point,
        // since either one alone is enough to protect the process.
        it('records the event and keeps serving when an extra hook throws', async () => {
            const ctx = await setup(supervisorOverrides(), {
                onEvent: () => { throw new Error('observer exploded'); },
            });
            const session = ctx.hub.registry.create('fanout');
            const worker = await connectWorker(
                `${ctx.wsBase}/agent/fanout/events?token=${encodeURIComponent(session.token)}`);
            try {
                worker.send(workerEvent({ session: 'fanout', event: 'status', data: { active: false } }));
                await until(() => session.stats.events > 0,
                    { timeout: 5000, label: 'the event to be recorded' });
                const alive = await fetch(`${ctx.base}/api/meta`);
                assert.equal(alive.status, 200, 'a throwing observer ended the hub');
            } finally {
                await worker.close();
            }
        });
    });

    describe('a superseded worker connection does not report a disconnect', () => {
        it('keeps the session connected when a newer worker takes over', async () => {
            const changes = [];
            const ctx = await setup(supervisorOverrides(), {
                onConnectionChange: (_session, connection) => {
                    changes.push(connection === null ? 'disconnected' : 'connected');
                },
            });
            const session = ctx.hub.registry.create('supersede');
            const url = `${ctx.wsBase}/agent/supersede/events?token=${encodeURIComponent(session.token)}`;
            const first = await connectWorker(url);
            await until(() => session.connection !== null, { timeout: 5000, label: 'the first connection' });
            const firstConnection = session.connection;

            const second = await connectWorker(url);
            await until(() => session.connection !== null && session.connection !== firstConnection,
                { timeout: 5000, label: 'the replacement connection' });

            // The superseded socket closes asynchronously, after its
            // replacement is already attached.
            await until(() => first.closed !== null, { timeout: 5000, label: 'the old socket to close' });
            await delay(100);

            assert.ok(!changes.includes('disconnected'),
                `a supersede was reported as a disconnect: ${changes.join(', ')}`);
            assert.equal(session.connected, true,
                'the session is reported disconnected while a live worker is attached');

            await second.close();
        });
    });

    describe('the CLI shuts down cleanly', () => {
        // `bin/` is the one module with no other test. This covers it end to
        // end: config load, listen, and the signal path. It does not on its own
        // prove that a rejected shutdown is handled — a healthy hub never
        // rejects — but it pins that the ordinary path exits zero and says
        // nothing about a failed shutdown.
        it('exits zero on SIGTERM instead of dying on a rejection', async () => {
            const child = spawn(process.execPath, [
                join(hubRoot, 'bin', 'simplex-hub.js'),
                '--listen', '127.0.0.1:0',
                '--data-dir', mkdtempSync(join(tmpdir(), 'simplex-hub-signal-')),
            ], { stdio: ['ignore', 'pipe', 'pipe'] });
            let output = '';
            let stderr = '';
            // The hub's logger writes to stderr, so the readiness line is looked
            // for across both streams.
            child.stdout.on('data', (chunk) => { output += chunk; });
            child.stderr.on('data', (chunk) => { output += chunk; stderr += chunk; });
            try {
                // The panel line carries the ephemeral port, so it doubles as
                // proof that the listener is up before the signal is sent.
                await until(() => /panel: http:\/\//.test(output),
                    { timeout: 20000, label: `the hub to listen; saw: ${output || '(nothing)'}` });
                child.kill('SIGTERM');
                const code = await new Promise((resolve) => child.once('exit', resolve));
                assert.equal(code, 0, `hub exited with ${code}; output: ${output}`);
                assert.doesNotMatch(stderr, /shutdown failed/, stderr);
            } finally {
                if (child.exitCode === null) child.kill('SIGKILL');
            }
        });
    });
});
