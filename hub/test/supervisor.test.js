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
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { after, describe, it } from 'node:test';
import { PROCESS_STATE, isSameProcess, readProcessStartTime } from '../src/launch/supervisor.ts';
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
        const written = JSON.parse(readFileSync(configPath, 'utf8'));
        assert.equal(written.client.endpoint, ctx.hub.supervisor.endpointsFor(session.id, session.token).events);
        assert.match(written.client.endpoint, new RegExp(`token=${session.token}`));
        assert.equal(written.driver_model, 'deepseek');
        assert.equal(written.persistence.directory, join(ctx.config.dataDir, 'sessions'));

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
        const logPath = join(sessionDir(ctx.config, session.id), 'worker.log');
        assert.ok(existsSync(logPath));
        await until(() => readFileSync(logPath, 'utf8').includes('fixture: connected'));
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
