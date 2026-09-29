/**
 * Opt-in host/Docker lifetime check for the documented container launcher.
 *
 * Run with SIMPLEX_DOCKER_WORKER_TEST=1 after building simplex-hub-test:latest.
 * The hub runs as the invoking (non-root) host user; the worker runs as root
 * inside Docker and must leave its state/memory tree removable by the hub.
 */
import assert from 'node:assert/strict';
import { existsSync, mkdtempSync, rmSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { createHub } from '../../src/hub.ts';
import { hubRoot, loadConfig } from '../../src/config.ts';
import { sessionDir } from '../../src/launch/config-render.ts';
import { createLogger } from '../../src/log.ts';
import { connectWorker, until } from '../helpers/worker.js';

const enabled = process.env.SIMPLEX_DOCKER_WORKER_TEST === '1';

describe('Docker worker host ownership', { skip: enabled ? false : 'opt-in Docker test' }, () => {
    it('compacts, stops, and deletes its session as a non-root hub', { timeout: 180000 }, async () => {
        assert.notEqual(process.getuid(), 0, 'the host hub must run as a non-root user');
        const dataDir = mkdtempSync(join(tmpdir(), 'simplex-docker-lifetime-'));
        const { config } = loadConfig({
            file: join(hubRoot, 'hub.config.docker-worker.jsonc'),
            overrides: {
                dataDir,
                listen: { host: '0.0.0.0', port: 0 },
                toolRequests: { port: 0 },
                panel: { token: 'docker-lifetime-test' },
            },
        });
        const hub = createHub({ config, log: createLogger({ level: 'silent' }),
            hubRoot, version: 'test' });
        let panel;
        try {
            const { port } = await hub.start();
            const base = `http://127.0.0.1:${port}`;
            const api = async (path, method, body) => {
                const response = await fetch(`${base}${path}`, {
                    method,
                    headers: { authorization: 'Bearer docker-lifetime-test',
                        'content-type': 'application/json' },
                    ...(body === undefined ? {} : { body: JSON.stringify(body) }),
                });
                return { status: response.status, body: await response.json() };
            };
            panel = await connectWorker(
                `ws://127.0.0.1:${port}/panel/ws?token=docker-lifetime-test`);
            const id = 'docker-lifetime';
            const created = await api('/api/sessions', 'POST', {
                session: id, spec: { provider: 'mock', model: 'mock-text' },
            });
            assert.equal(created.status, 201, JSON.stringify(created.body));
            const started = await api(`/api/sessions/${id}/start`, 'POST');
            assert.equal(started.body.ok, true, JSON.stringify(started.body));
            const session = hub.registry.get(id);
            await until(() => session.connected &&
                session.workerCapabilities?.names.includes('context-compact'),
            { timeout: 60000, label: 'Docker worker compact capability' });
            panel.send({ v: 1, type: 'subscribe', session: id });
            await panel.waitFor((message) => message.type === 'subscribed');
            panel.send({ v: 1, type: 'input', session: id, request_id: 'message',
                content: [{ type: 'text', raw: 'A detailed historical note. '.repeat(2000) }] });
            await panel.waitFor((message) => message.type === 'event'
                && message.envelope.event === 'run_finished'
                && message.envelope.request_id === 'message', { timeout: 60000 });
            panel.send({ v: 1, type: 'input', session: id, request_id: 'compact',
                operation: 'compact' });
            const compact = await panel.waitFor((message) => message.type === 'event'
                && message.envelope.event === 'compact_finished'
                && message.envelope.request_id === 'compact', { timeout: 60000 });
            assert.equal(compact.envelope.data.durable, true);
            const directory = sessionDir(config, id);
            for (const name of ['state', 'memory']) {
                const mode = statSync(join(directory, name)).mode;
                assert.ok(mode & 0o020, `${name} must be writable by the hub group`);
            }
            const stopped = await api(`/api/sessions/${id}/stop`, 'POST');
            assert.equal(stopped.body.ok, true, JSON.stringify(stopped.body));
            const deleted = await api(`/api/sessions/${id}`, 'DELETE');
            assert.equal(deleted.status, 200, JSON.stringify(deleted.body));
            assert.equal(deleted.body.removed, id);
            assert.equal(existsSync(directory), false);
        } finally {
            if (panel) await panel.close();
            await hub.stop();
            rmSync(dataDir, { recursive: true, force: true });
        }
    });
});
