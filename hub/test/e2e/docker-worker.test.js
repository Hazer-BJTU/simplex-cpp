/**
 * Opt-in acceptance for a managed worker inside an operator-retained container.
 *
 * The Hub does not create, start, stop or remove the container. Provide one that
 * is already running, contains an installation of this revision (`simplex run`
 * and `simplex manage`), and can reach the Hub listeners. Host networking keeps
 * the example simple:
 *
 *   docker run -d --name simplex-managed-e2e --network host \
 *       --entrypoint sleep simplex-hub-test:latest infinity
 *   SIMPLEX_DOCKER_WORKER_TEST=1 \
 *   SIMPLEX_DOCKER_MANAGED_CONTAINER=simplex-managed-e2e \
 *   SIMPLEX_DOCKER_MANAGED_EXECUTABLE=/src/build/bin/simplex \
 *   node --test test/e2e/docker-worker.test.js
 *
 * `SIMPLEX_DOCKER_MANAGED_ROOT` (container-local allocation root, default
 * `/var/lib/simplex-test/allocations`) and `SIMPLEX_DOCKER` (Docker CLI) are
 * optional. The Hub listens on all interfaces because worker routes must be
 * reachable from the container; the panel token is a test value.
 */
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { describe, it } from 'node:test';
import { createHub } from '../../src/hub.ts';
import { hubRoot, loadConfig } from '../../src/config.ts';
import { createLogger } from '../../src/log.ts';
import { ConfigurationStore } from '../../src/configurations/store.ts';
import { snapshotConfigs } from '../../src/configurations/session.ts';
import { readManagedSession } from '../../src/launch/managed-sessions.ts';
import { connectWorker, until } from '../helpers/worker.js';

const enabled = process.env.SIMPLEX_DOCKER_WORKER_TEST === '1';
const container = process.env.SIMPLEX_DOCKER_MANAGED_CONTAINER ?? '';
const docker = process.env.SIMPLEX_DOCKER ?? 'docker';
const executable = process.env.SIMPLEX_DOCKER_MANAGED_EXECUTABLE ?? '/src/build/bin/simplex';
const managementRoot = process.env.SIMPLEX_DOCKER_MANAGED_ROOT ?? '/var/lib/simplex-test/allocations';

describe('Managed Docker worker in a retained container',
    { skip: enabled && container ? false : 'set SIMPLEX_DOCKER_WORKER_TEST=1 and SIMPLEX_DOCKER_MANAGED_CONTAINER' }, () => {
        it('starts, stops and retires a worker without touching the container', { timeout: 180000 }, async () => {
            const dockerCli = args => spawnSync(docker, args, { encoding: 'utf8', timeout: 30000 });
            const running = dockerCli(['inspect', '--format', '{{.State.Running}}', container]);
            assert.equal(running.status, 0, running.stderr);
            assert.equal(running.stdout.trim(), 'true', `container ${container} must already be running`);

            const dataDir = mkdtempSync(join(tmpdir(), 'simplex-docker-managed-'));
            const { config } = loadConfig({ overrides: {
                dataDir,
                listen: { host: '0.0.0.0', port: 0 },
                toolRequests: { port: 0 },
                panel: { token: 'docker-managed-test' },
                worker: { connectHost: '127.0.0.1', threads: 1, stopTimeoutMs: 10000 },
                mock: { enabled: true },
            } });
            const hub = createHub({ config, log: createLogger({ level: 'silent' }), hubRoot, version: 'test' });
            const id = 'docker-managed';
            let panel;
            try {
                const { port } = await hub.start();
                const base = `http://127.0.0.1:${port}`;
                const api = async (path, method, body) => {
                    const response = await fetch(`${base}${path}`, { method,
                        headers: { authorization: 'Bearer docker-managed-test', 'content-type': 'application/json' },
                        ...(body === undefined ? {} : { body: JSON.stringify(body) }) });
                    return { status: response.status, body: await response.json() };
                };
                const store = new ConfigurationStore(config);
                store.save('launch', 'docker-managed', JSON.stringify({
                    management: {
                        target: { kind: 'docker', command: docker, args: [], container },
                        executable,
                        cwd: dataDir,
                        env: {},
                        root: managementRoot,
                    },
                    worker: { threads: 1, stopTimeoutMs: 10000 },
                }), null);
                snapshotConfigs(store, id, { launchConfig: 'docker-managed', workerConfig: 'default' });
                hub.registry.create(id, { launchConfig: 'docker-managed', workerConfig: 'default' });
                panel = await connectWorker(`ws://127.0.0.1:${port}/panel/ws?token=docker-managed-test`);

                const started = await api(`/api/sessions/${id}/start`, 'POST');
                assert.equal(started.body.ok, true, JSON.stringify(started.body));
                const session = hub.registry.get(id);
                await until(() => session.connected && session.workerCapabilities?.names.includes('context-compact'),
                    { timeout: 60000, label: 'managed Docker worker capability' });
                const record = readManagedSession(config, id);
                assert.ok(record, 'the managed allocation record must exist');
                const inside = dockerCli(['exec', container, 'test', '-d', record.reference.directory]);
                assert.equal(inside.status, 0, 'the allocation must exist inside the container');

                const stopped = await api(`/api/sessions/${id}/stop`, 'POST');
                assert.equal(stopped.body.ok, true, JSON.stringify(stopped.body));
                const stillRunning = dockerCli(['inspect', '--format', '{{.State.Running}}', container]);
                assert.equal(stillRunning.stdout.trim(), 'true', 'the Hub must not stop an operator-retained container');

                const deleted = await api(`/api/sessions/${id}`, 'DELETE');
                assert.equal(deleted.status, 200, JSON.stringify(deleted.body));
                const retired = dockerCli(['exec', container, 'test', '-d', record.reference.directory]);
                assert.notEqual(retired.status, 0, 'the manager must retire the allocation');
            } finally {
                if (panel) await panel.close();
                await hub.stop();
                rmSync(dataDir, { recursive: true, force: true });
            }
        });
    });
