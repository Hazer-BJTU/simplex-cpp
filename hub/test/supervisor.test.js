/**
 * @file worker process supervision, driven by a stand-in worker process.
 *
 * The fixture connects back over a real WebSocket using the configuration the
 * hub generated, which is what makes these tests meaningful: they cover the
 * whole path from a session spec to a running, reachable worker, including the
 * protocol-first stop and the escalation that follows a worker that refuses to
 * leave.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { syncBuiltinESMExports } from 'node:module';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { parse, stringify } from 'yaml';
import { join } from 'node:path';
import { after, describe, it } from 'node:test';
import { PROCESS_STATE, isSameProcess, processIdentity, readProcessStartTime } from '../src/launch/supervisor.ts';
import { sessionDir, workerConfigPath } from '../src/launch/config-render.ts';
import { until } from './helpers/worker.js';
import { startTestHub } from './helpers/hub.js';

const FIXTURE = join(import.meta.dirname, 'fixtures', 'fake-worker.js');

/** Hub configuration that spawns the fixture instead of the C++ worker. */
function supervisorOverrides(extra = {}) {
    return {
        worker: {
            bin: FIXTURE,
            stopTimeoutMs: 100,
            sigtermGraceMs: 200,
            sigkillGraceMs: 1000,
            ...extra,
        },
    };
}

describe('worker supervisor', () => {
    const hubs = [];

    /** Start a hub plus a session, and remember the hub for cleanup. */
    async function setup(overrides = supervisorOverrides(), spec = {}) {
        const ctx = await startTestHub(overrides);
        hubs.push(ctx);
        const session = ctx.hub.registry.create(`sup-${hubs.length}-${Math.floor(Math.random() * 1e6)}`);
        session.spec = spec;
        return { ctx, session };
    }

    after(async () => {
        for (const ctx of hubs) await ctx.hub.stop();
    });

    it('starts a worker that connects back with the generated configuration', async () => {
        const { ctx, session } = await setup();
        const started = await ctx.hub.supervisor.start(session);
        assert.equal(started.ok, true, started.error);
        assert.ok(Number.isInteger(started.pid) && started.pid > 0);
        assert.equal(session.process.state, PROCESS_STATE.running);
        assert.equal(session.process.pid, started.pid);
        assert.equal(typeof session.process.pidStartTime, 'string');

        const configPath = workerConfigPath(ctx.config, session.id);
        assert.ok(existsSync(configPath));
        const written = parse(readFileSync(configPath, 'utf8'));
        assert.equal(written.client.endpoint, ctx.hub.supervisor.endpointsFor(session.id, session.token).events);
        assert.match(written.client.endpoint, new RegExp(`token=${session.token}`));
        assert.equal(written.driver_model, 'deepseek');
        assert.equal(written.persistence.directory, join(ctx.config.dataDir, 'sessions', session.id));

        // The fixture reports `ready` and answers the hub's status request, so
        // the session must end up with a live identity.
        await until(() => session.identity.state === 'live', { label: 'live worker identity', timeout: 5000 });
        assert.match(session.identity.workerId, /^fixture-/);
        // The fixture answers the hub's status request; wait for that reply
        // instead of assuming it landed with the identity.
        await until(() => session.latest.status !== null, { label: 'status event', timeout: 5000 });
        assert.equal(session.latest.status.data.active, false);
    });

    it('captures worker output in memory and on disk', async () => {
        const { ctx, session } = await setup();
        await ctx.hub.supervisor.start(session);
        await until(() => ctx.hub.supervisor.logs(session).some(
            (line) => line.includes('fixture: connected')), { label: 'worker log lines', timeout: 5000 });
        const lines = ctx.hub.supervisor.logs(session);
        assert.ok(lines.some((line) => line.includes(`session=${session.id}`)));
        assert.ok(lines.some((line) => line.includes('stderr line')));
        const logPath = join(sessionDir(ctx.config, session.id), 'logs', 'worker.log');
        assert.ok(existsSync(logPath));
        await until(() => readFileSync(logPath, 'utf8').includes('fixture: connected'));
    });

    it('preserves interleaved UTF-8 bytes and both EOF fragments from a real child', async () => {
        const { ctx, session } = await setup();
        await ctx.hub.supervisor.start(session);
        await until(() => session.latest.status !== null);
        const probe = (stage) => session.connection.send({
            type: 'signal', data: { operation: 'log_probe', stage },
        });
        probe('prefix');
        await until(() => ctx.hub.supervisor.logs(session).includes('fixture: independent stderr'));
        // The second stdout fragment is sent only after stderr was captured.
        probe('finish');
        await until(() => session.process.state === PROCESS_STATE.exited);
        await until(() => ctx.hub.supervisor.logs(session).includes('fixture: stderr EOF'));
        const lines = ctx.hub.supervisor.logs(session);
        assert.ok(lines.includes('中'));
        assert.ok(lines.includes('fixture: stdout EOF�'));
        assert.equal(lines.some((line) => line.includes('�fixture: independent stderr')), false);
        const logPath = join(sessionDir(ctx.config, session.id), 'logs', 'worker.log');
        await until(() => readFileSync(logPath, 'utf8').includes('fixture: stdout EOF�\n'));
        assert.match(readFileSync(logPath, 'utf8'), /fixture: stderr EOF\n/);
    });

    it('bounds a real child partial line while status routing remains responsive', async () => {
        const { ctx, session } = await setup();
        await ctx.hub.supervisor.start(session);
        await until(() => session.latest.status !== null);
        session.connection.send({ type: 'signal', data: { operation: 'log_probe', stage: 'long' } });
        await until(() => session.process.outputSplitters[0].truncatedBytes >= 192 * 1024);
        assert.equal(session.process.outputSplitters[0].pendingBytes, 64 * 1024);
        const previousStatus = session.latest.status;
        session.connection.send({ type: 'signal', data: { operation: 'status' } });
        await until(() => session.latest.status !== previousStatus);
        await ctx.hub.supervisor.stop(session);
        await until(() => ctx.hub.supervisor.logs(session).some((line) => line.includes('[hub: truncated')));
        assert.ok(session.process.describe().log_truncated_bytes >= 192 * 1024);
    });

    it('keeps worker control and live events usable after both optional log files fail', async () => {
        const { ctx, session } = await setup();
        const directory = sessionDir(ctx.config, session.id);
        // Directories at the expected file paths force portable async open errors.
        mkdirSync(join(directory, 'logs', 'worker.log'), { recursive: true });
        mkdirSync(join(directory, 'events.jsonl'), { recursive: true });
        const started = await ctx.hub.supervisor.start(session);
        assert.equal(started.ok, true, started.error);
        await until(() => session.latest.status !== null);
        const transcript = ctx.hub.transcripts.get(session.id);
        await until(() => session.process.logStream.failed && transcript.writer.failed);
        const previousStatus = session.latest.status;
        session.connection.send({ type: 'signal', data: { operation: 'status' } });
        await until(() => session.latest.status !== previousStatus);
        assert.ok(transcript.toArray().some((item) => item.event === 'status'));
        assert.equal(session.process.describe().file_log_failed, true);
        assert.ok(ctx.hub.supervisor.logs(session).some((line) => line.includes('fixture: connected')));
        assert.ok(transcript.writer.droppedRecords > 0);
        const stopped = await ctx.hub.supervisor.stop(session);
        assert.equal(stopped.ok, true);
    });

    it('stops a worker with the protocol before signalling it', async () => {
        const { ctx, session } = await setup();
        await ctx.hub.supervisor.start(session);
        await until(() => session.connected, { label: 'worker connection' });
        const stopped = await ctx.hub.supervisor.stop(session);
        assert.deepEqual(stopped, { ok: true, how: 'shutdown-signal', forced: false });
        assert.equal(session.process.state, PROCESS_STATE.exited);
        assert.equal(session.process.exitCode, 0);
        assert.equal(ctx.hub.supervisor.logs(session).some(
            (line) => line.includes('fixture: shutdown signal')), true);
    });

    it('refuses to start a second worker for a running session', async () => {
        const { ctx, session } = await setup();
        await ctx.hub.supervisor.start(session);
        const second = await ctx.hub.supervisor.start(session);
        assert.equal(second.ok, false);
        assert.match(second.error, /already running/);
    });

    it('restarts with a new process after the previous one exits', async () => {
        const { ctx, session } = await setup();
        const first = await ctx.hub.supervisor.start(session);
        await until(() => session.connected, { label: 'first worker connection' });
        const restarted = await ctx.hub.supervisor.restart(session);
        assert.equal(restarted.ok, true, restarted.error);
        assert.equal(restarted.stop, 'shutdown-signal');
        assert.notEqual(restarted.pid, first.pid);
        assert.equal(session.process.state, PROCESS_STATE.running);
        await until(() => session.connected, { label: 'second worker connection' });
    });

    it('restarts with operator-edited YAML and refreshed session credentials', async () => {
        const { ctx, session } = await setup();
        const first = await ctx.hub.supervisor.start(session);
        assert.equal(first.ok, true, first.error);
        await until(() => session.connected);
        await ctx.hub.supervisor.stop(session);
        await until(() => !session.connected);
        const path = workerConfigPath(ctx.config, session.id);
        const saved = parse(readFileSync(path, 'utf8'));
        saved.worker.max_exchanges = 77;
        saved.client.endpoint = 'ws://127.0.0.1:1/stale';
        writeFileSync(path, '# Keep this operator comment\n' + stringify(saved));
        session.token = 'replacement-token';
        const second = await ctx.hub.supervisor.start(session);
        assert.equal(second.ok, true, second.error);
        await until(() => session.connected);
        const current = parse(readFileSync(path, 'utf8'));
        assert.equal(current.worker.max_exchanges, 77);
        assert.match(current.client.endpoint, /replacement-token/);
        assert.match(readFileSync(path, 'utf8'), /Keep this operator comment/);
    });

    it('escalates to SIGTERM and then SIGKILL for a worker that refuses to leave', async () => {
        const overrides = supervisorOverrides();
        const ctx = await startTestHub(overrides);
        hubs.push(ctx);
        const session = ctx.hub.registry.create('sup-hung');
        const started = await ctx.hub.supervisor.start(session, { extraArgs: ['--hang', '--ignore-shutdown'] });
        assert.equal(started.ok, true, started.error);
        // start() returns after spawn, before the fixture has installed its
        // SIGTERM handler. Its event connection proves startup is complete.
        await until(() => session.connected, { label: 'hung worker connection' });
        const stopped = await ctx.hub.supervisor.stop(session);
        assert.equal(stopped.ok, true);
        assert.equal(stopped.forced, true);
        // The fixture ignores SIGTERM, so only SIGKILL can end it; with the
        // process-group option off the signal still goes to the process alone.
        assert.equal(stopped.how, 'sigkill');
        assert.equal(session.process.state, PROCESS_STATE.exited);
    });

    it('force-kills a stuck worker through its process group', async () => {
        const overrides = supervisorOverrides();
        const ctx = await startTestHub(overrides);
        hubs.push(ctx);
        const session = ctx.hub.registry.create('sup-force');
        await ctx.hub.supervisor.start(session, { extraArgs: ['--hang', '--ignore-shutdown'] });
        await until(() => session.process.state === PROCESS_STATE.running);
        const killed = await ctx.hub.supervisor.forceKill(session);
        assert.equal(killed.ok, true);
        assert.equal(killed.how, 'sigkill-process-group');
        assert.equal(session.process.processGroupKilled, true);
    });

    it('reports an invalid session spec without spawning anything', async () => {
        const { ctx, session } = await setup(supervisorOverrides(), {});
        const result = await ctx.hub.supervisor.start(session, { provider: 'nope' });
        assert.equal(result.ok, false);
        assert.match(result.error, /unknown provider profile/);
        // Nothing was rendered and nothing was spawned.
        assert.equal(session.process, null);
    });

    it('reports a spawn failure instead of pretending the worker started', async () => {
        const { ctx, session } = await setup(supervisorOverrides({ bin: '/nonexistent/simplex_worker' }));
        const result = await ctx.hub.supervisor.start(session);
        assert.equal(result.ok, false);
        assert.match(result.error, /cannot spawn/);
        assert.equal(session.process.state, PROCESS_STATE.failed);
        assert.match(session.process.error, /ENOENT/);
    });

    it('runs a template launcher with expanded placeholders', async () => {
        const ctx = await startTestHub({
            worker: { stopTimeoutMs: 100, sigtermGraceMs: 200, sigkillGraceMs: 1000 },
            launcher: {
                kind: 'command',
                command: [process.execPath, FIXTURE, '--config', '{config}', '--session', '{session}'],
                args: ['--data-dir', '{data_dir}'],
            },
        });
        hubs.push(ctx);
        const session = ctx.hub.registry.create('sup-command');
        const started = await ctx.hub.supervisor.start(session);
        assert.equal(started.ok, true, started.error);
        assert.equal(session.process.command, process.execPath);
        assert.deepEqual(session.process.args.slice(0, 2), [FIXTURE, '--config']);
        await until(() => session.identity.state === 'live', { label: 'fixture identity', timeout: 5000 });
        const stopped = await ctx.hub.supervisor.stop(session);
        assert.equal(stopped.how, 'shutdown-signal');
    });
});

describe('process identity helpers', () => {
    it('does not mistake a hidden procfs entry for termination', () => {
        const startTime = readProcessStartTime(process.pid);
        const original = fs.readFileSync;
        fs.readFileSync = (path, ...args) => {
            if (path === `/proc/${process.pid}/stat`) {
                throw Object.assign(new Error('hidden proc entry'), { code: 'ENOENT' });
            }
            return original(path, ...args);
        };
        syncBuiltinESMExports();
        try { assert.equal(processIdentity(process.pid, startTime), 'unknown'); }
        finally { fs.readFileSync = original; syncBuiltinESMExports(); }
    });
    it('keeps missing or malformed identity unknown instead of claiming termination', () => {
        assert.equal(processIdentity(process.pid, null), 'unknown');
        assert.equal(processIdentity(process.pid, 'unavailable'), 'unknown');
        assert.equal(processIdentity(-1, '123'), 'unknown');
        assert.equal(processIdentity(2147483647, null), 'unknown');
        if (process.platform === 'linux') {
            assert.equal(processIdentity(2147483647, '123'), 'gone');
            assert.equal(processIdentity(process.pid, readProcessStartTime(process.pid)), 'same');
        }
    });
    it('reads a start time for a live process', () => {
        const startTime = readProcessStartTime(process.pid);
        assert.equal(typeof startTime, 'string');
        assert.match(startTime, /^\d+$/);
    });

    it('distinguishes the recorded incarnation from a different one', () => {
        const startTime = readProcessStartTime(process.pid);
        assert.equal(isSameProcess(process.pid, startTime), true);
        assert.equal(isSameProcess(process.pid, '1'), false);
        assert.equal(isSameProcess(999999, null), false);
        assert.equal(isSameProcess(0, null), false);
    });
});
