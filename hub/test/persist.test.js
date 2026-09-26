/**
 * @file durable hub state: what survives a hub restart, and what must not be
 * trusted when it does.
 */
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { HubState, STATE_VERSION } from '../src/state/persist.ts';
import { PROCESS_STATE, readProcessStartTime } from '../src/launch/supervisor.js';
import { createLogger } from '../src/log.ts';
import { loadConfig } from '../src/config.js';
import { createHub } from '../src/hub.js';
import { hubRoot } from '../src/config.js';
import { until } from './helpers/worker.js';

const log = createLogger({ level: 'silent' });

/** Configuration with a scratch data directory. */
function scratch() {
    const dataDir = mkdtempSync(join(tmpdir(), 'simplex-hub-state-'));
    const { config } = loadConfig({
        overrides: { listen: { host: '127.0.0.1', port: 0 }, dataDir },
    });
    return { config, dataDir };
}

describe('HubState', () => {
    it('round-trips sessions, tokens, and process identity', () => {
        const { config, dataDir } = scratch();
        const state = new HubState({ config, log });
        assert.deepEqual(state.load(), { version: STATE_VERSION, sessions: [] });

        state.save([{
            id: 'demo',
            token: 'token-value',
            spec: { provider: 'mock', threads: 2 },
            createdAt: '2026-01-01T00:00:00.000Z',
            process: {
                pid: 4242,
                pidStartTime: '12345',
                startedAt: '2026-01-01T00:00:01.000Z',
                command: '/bin/true',
                args: ['--x'],
                cwd: '/tmp',
                pidFile: null,
                logPath: '/tmp/worker.log',
            },
        }]);
        const stored = JSON.parse(readFileSync(join(dataDir, 'hub.json'), 'utf8'));
        assert.equal(stored.version, STATE_VERSION);
        assert.equal(stored.sessions[0].token, 'token-value');

        const reloaded = new HubState({ config, log }).load();
        assert.equal(reloaded.sessions.length, 1);
        assert.equal(reloaded.sessions[0].id, 'demo');
        assert.equal(reloaded.sessions[0].spec.provider, 'mock');
        assert.equal(reloaded.sessions[0].process.pid, 4242);
    });

    it('treats a corrupt state file as empty instead of refusing to start', () => {
        const { config, dataDir } = scratch();
        writeFileSync(join(dataDir, 'hub.json'), '{ not json');
        const state = new HubState({ config, log });
        assert.deepEqual(state.load().sessions, []);
    });

    it('writes atomically and leaves no temporary file behind', () => {
        const { config, dataDir } = scratch();
        const state = new HubState({ config, log });
        state.save([{ id: 'demo', token: 't', spec: {}, createdAt: 'x', process: null }]);
        assert.equal(existsSync(join(dataDir, 'hub.json.tmp')), false);
        assert.equal(JSON.parse(readFileSync(join(dataDir, 'hub.json'), 'utf8')).sessions.length, 1);
    });

    it('collapses scheduled saves and flushes on demand', async () => {
        const { config, dataDir } = scratch();
        const state = new HubState({ config, log });
        state.schedule([{ id: 'first', token: 'a', spec: {}, createdAt: 'x', process: null }]);
        state.schedule([{ id: 'second', token: 'b', spec: {}, createdAt: 'x', process: null }]);
        await until(() => existsSync(join(dataDir, 'hub.json')), { label: 'debounced save' });
        const stored = JSON.parse(readFileSync(join(dataDir, 'hub.json'), 'utf8'));
        assert.deepEqual(stored.sessions.map((entry) => entry.id), ['second']);

        state.schedule([{ id: 'third', token: 'c', spec: {}, createdAt: 'x', process: null }]);
        assert.equal(state.flush(), true);
        assert.deepEqual(
            JSON.parse(readFileSync(join(dataDir, 'hub.json'), 'utf8')).sessions.map((e) => e.id),
            ['third']);
    });
});

describe('hub restart', () => {
    /** Start a hub over an existing configuration. */
    async function start(config) {
        const hub = createHub({ config, log, hubRoot, version: 'test' });
        await hub.start();
        return hub;
    }

    it('restores sessions with their tokens so a running worker can reconnect', async () => {
        const { config, dataDir } = scratch();
        const first = await start(config);
        const created = first.registry.create('survivor', { provider: 'mock' });
        created.spec = { provider: 'mock', threads: 3 };
        first.registry.create('idle');
        first.panel.hooks.onProcessChange(created, null);
        await until(() => existsSync(join(dataDir, 'hub.json')), { label: 'state written' });
        const token = created.token;
        await first.stop();

        const second = await start(config);
        try {
            const restored = second.registry.get('survivor');
            assert.ok(restored, 'session was not restored');
            assert.equal(restored.token, token);
            assert.equal(restored.spec.threads, 3);
            assert.equal(restored.spec.provider, 'mock');
            assert.ok(second.registry.get('idle'));
            assert.equal(restored.connected, false);
        } finally {
            await second.stop();
        }
    });

    it('adopts a worker recorded by a previous run only when the pid still matches', async () => {
        const { config, dataDir } = scratch();
        const pid = process.pid;
        const startTime = readProcessStartTime(pid);
        writeFileSync(join(dataDir, 'hub.json'), JSON.stringify({
            version: STATE_VERSION,
            sessions: [
                {
                    id: 'live-orphan',
                    token: 'keep-me',
                    spec: {},
                    created_at: '2026-01-01T00:00:00.000Z',
                    process: {
                        pid,
                        pid_start_time: startTime,
                        started_at: '2026-01-01T00:00:00.000Z',
                        command: '/bin/true',
                        args: [],
                        cwd: '/tmp',
                        pid_file: null,
                        log_path: null,
                    },
                },
                {
                    id: 'stale-pid',
                    token: 'keep-me-too',
                    spec: {},
                    created_at: '2026-01-01T00:00:00.000Z',
                    process: {
                        pid,
                        // A different incarnation of the same pid.
                        pid_start_time: '1',
                        started_at: '2026-01-01T00:00:00.000Z',
                        command: '/bin/true',
                        args: [],
                        cwd: '/tmp',
                        pid_file: null,
                        log_path: null,
                    },
                },
            ],
        }));

        const hub = await start(config);
        let adoptedRecord = null;
        try {
            const adopted = hub.registry.get('live-orphan');
            assert.equal(adopted.process.state, PROCESS_STATE.running);
            assert.equal(adopted.process.adopted, true);
            assert.equal(adopted.process.pid, pid);
            assert.equal(hub.supervisor.isRunning(adopted), true);
            adoptedRecord = adopted.process;

            // Nothing may be signalled for a pid that was reused: the recorded
            // start time no longer matches the process now holding that pid.
            const stale = hub.registry.get('stale-pid');
            assert.equal(stale.process, null);
            assert.equal(stale.token, 'keep-me-too');
        } finally {
            // This record deliberately points at the test process, so it must
            // not be supervised any further: in a real restart the pid would
            // belong to a worker, and stopping it would be the point.
            if (adoptedRecord) {
                clearInterval(adoptedRecord.monitor);
                adoptedRecord.monitor = null;
                adoptedRecord.state = PROCESS_STATE.exited;
            }
            await hub.stop();
        }
    });

    it('ignores unusable stored entries instead of failing startup', async () => {
        const { config, dataDir } = scratch();
        writeFileSync(join(dataDir, 'hub.json'), JSON.stringify({
            version: STATE_VERSION,
            sessions: [
                { id: '../escape', token: 'x', spec: {} },
                { id: 'fine', token: 'y', spec: {} },
                { token: 'no-id', spec: {} },
            ],
        }));
        const hub = await start(config);
        try {
            assert.deepEqual(hub.registry.list().map((session) => session.id), ['fine']);
        } finally {
            await hub.stop();
        }
    });
});
