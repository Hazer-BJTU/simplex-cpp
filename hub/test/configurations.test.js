import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { defaultConfig, parseConfigText } from '../src/config.ts';
import { ConfigurationStore } from '../src/configurations/store.ts';

test('configuration library preserves text, guards revisions and survives restart', () => {
    const dataDir = mkdtempSync(join(tmpdir(), 'simplex-config-'));
    try {
        const config = { ...defaultConfig(), dataDir };
        const store = new ConfigurationStore(config);
        const initial = store.read('worker', 'default');
        assert.match(initial.text, /\{\{hub.events_endpoint\}\}/);
        const edited = '# Operator comment\n' + initial.text;
        const saved = store.save('worker', 'default', edited, initial.revision);
        assert.throws(() => store.save('worker', 'default', initial.text, initial.revision), /changed/);
        assert.equal(new ConfigurationStore(config).read('worker', 'default').text, edited);
        store.save('worker', 'copy', edited, null);
        assert.throws(() => store.read('worker', '../escape'), /Invalid/);
        assert.throws(() => store.save('worker', 'bad', 'driver_model: absent', null), /providers/);
        assert.throws(() => store.remove('worker', 'default', initial.revision), /changed/);
        store.remove('worker', 'copy', saved.revision);
        assert.deepEqual(store.list('worker'), ['default']);
    } finally { rmSync(dataDir, { recursive: true, force: true }); }
});

test('bundled worker template stays aligned with canonical load template', () => {
    assert.equal(readFileSync(new URL('../schemas/worker.yaml', import.meta.url), 'utf8'),
        readFileSync(new URL('../../load/schemas/config.example.yaml', import.meta.url), 'utf8'));
});

import { snapshotConfigs, sessionLaunch, launchEndpoints } from '../src/configurations/session.ts';
import { prepareSessionConfig } from '../src/launch/config-file.ts';
import { parseDocument } from 'yaml';

test('session snapshots preserve selected files, optional omissions and live endpoints', () => {
    const dataDir = mkdtempSync(join(tmpdir(), 'simplex-config-'));
    try {
        const config = { ...defaultConfig(), dataDir };
        const store = new ConfigurationStore(config);
        const worker = store.read('worker', 'default');
        const doc = parseDocument(worker.text);
        doc.delete('modality_assist_model');
        doc.delete('security');
        doc.delete('hub_remote_call');
        doc.set('custom_future_field', { kept: true });
        store.save('worker', 'default', doc.toString(), worker.revision);
        snapshotConfigs(store, 'demo', { launchConfig: 'local', workerConfig: 'default' });
        const launch = sessionLaunch(config, 'demo');
        assert.equal(launch.launch.launcher.command[0], 'simplex');
        store.remove('launch', 'local', store.read('launch', 'local').revision);
        assert.equal(sessionLaunch(config, 'demo').launch.launcher.command[0], 'simplex');
        const endpoints = { events: 'ws://localhost:1234/agent/demo/events?token=x', confirm: 'ws://localhost:1234/agent/demo/confirm?token=x', tools: 'ws://localhost:1235/agent/demo/tools?token=x' };
        const rendered = prepareSessionConfig({ config, sessionId: 'demo', rawSpec: {}, endpoints });
        assert.equal(rendered.document.client.endpoint, endpoints.events);
        assert.equal(rendered.document.persistence.directory, join(dataDir, 'sessions/demo'));
        assert.equal(rendered.document.security, undefined);
        assert.equal(rendered.document.hub_remote_call, undefined);
        assert.equal(rendered.spec.modalityAssistProvider, null);
        assert.deepEqual(rendered.document.custom_future_field, { kept: true });
        const proxy = launchEndpoints(endpoints, { ...launch.launch, endpoints: { events: 'wss://example.test/prefix' } });
        assert.equal(proxy.events, 'wss://example.test/prefix/agent/demo/events?token=x');
        assert.equal(proxy.tools, endpoints.tools);
    } finally { rmSync(dataDir, { recursive: true, force: true }); }
});

import { startTestHub } from './helpers/hub.js';

test('authenticated configuration API and session selection retain independent snapshots', async () => {
    const ctx = await startTestHub({ panel: { token: 'test-secret' } });
    const api = async (path, method = 'GET', body) => {
        const response = await fetch(ctx.base + path, {
            method, headers: { Authorization: 'Bearer test-secret', 'Content-Type': 'application/json' },
            body: body === undefined ? undefined : JSON.stringify(body),
        });
        return { status: response.status, body: await response.json() };
    };
    try {
        assert.equal((await fetch(ctx.base + '/api/configurations')).status, 401);
        assert.deepEqual((await api('/api/configurations')).body, { launch: ['local'], worker: ['default'] });
        const file = (await api('/api/configurations/launch/local')).body;
        const preview = await api('/api/configurations/preview', 'POST', { launch: file.text });
        assert.equal(new URL(preview.body.endpoints.events).port, String(ctx.port));
        const selected = { launchConfig: 'local', workerConfig: 'default' };
        assert.equal((await api('/api/sessions', 'POST', { session: 'selected', spec: selected })).status, 201);
        const path = join(ctx.config.dataDir, 'sessions/selected/config/launch.jsonc');
        const snapshot = readFileSync(path, 'utf8');
        assert.equal((await api('/api/configurations/launch/local', 'PUT', { text: '// Changed\n' + file.text, revision: file.revision })).status, 200);
        assert.equal(readFileSync(path, 'utf8'), snapshot);
        assert.equal((await api('/api/sessions/selected/configurations', 'POST', selected)).status, 200);
        assert.match(readFileSync(path, 'utf8'), /^\/\/ Changed/);
        const session = ctx.hub.registry.get('selected');
        session.process = { state: 'running' };
        assert.equal((await api('/api/sessions/selected/configurations', 'POST', selected)).status, 409);
        session.process = null;
    } finally {
        await ctx.hub.stop();
        rmSync(ctx.config.dataDir, { recursive: true, force: true });
    }
});

import { fileURLToPath } from 'node:url';
import { until } from './helpers/worker.js';

test('selected launchers run independently and survive Hub restart without library files', async () => {
    let ctx = await startTestHub();
    const dataDir = ctx.config.dataDir;
    try {
        const store = new ConfigurationStore(ctx.config);
        for (const [name, threads] of [['one', 2], ['two', 3]]) {
            const launch = parseConfigText(store.template('launch'), 'template');
            launch.launcher.command = [process.execPath, fileURLToPath(new URL('./fixtures/fake-worker.js', import.meta.url)),
                '--config', '{config}', '--session', '{session}', '--threads', '{threads}'];
            launch.worker.threads = threads;
            launch.env = { CONFIG_TEST_SECRET: 'not-in-session-metadata' };
            store.save('launch', name, JSON.stringify(launch), null);
            snapshotConfigs(store, name, { launchConfig: name, workerConfig: 'default' });
            const session = ctx.hub.registry.create(name, { launchConfig: name, workerConfig: 'default' });
            const result = await ctx.hub.supervisor.start(session);
            assert.equal(result.ok, true, result.error);
            assert.equal(JSON.stringify(result).includes('not-in-session-metadata'), false);
            assert.equal(JSON.stringify(session.describe()).includes('not-in-session-metadata'), false);
            await until(() => session.connected, { label: `${name} worker connects` });
            // The observable spec and spawned process both originate in the selected snapshot.
            assert.equal(session.spec.threads, threads);
            assert.ok(session.process.pid > 0);
            assert.equal((await ctx.hub.supervisor.stop(session)).ok, true);
            store.remove('launch', name, store.read('launch', name).revision);
        }
        await ctx.hub.stop();
        ctx = await startTestHub({ dataDir });
        const restored = ctx.hub.registry.get('one');
        assert.ok(restored);
        assert.equal(restored.spec.launchConfig, 'one');
        const result = await ctx.hub.supervisor.start(restored);
        assert.equal(result.ok, true, result.error);
        await until(() => restored.connected, { label: 'restored worker connects' });
        assert.equal(restored.spec.threads, 2);
        assert.equal((await ctx.hub.supervisor.stop(restored)).ok, true);
    } finally {
        await ctx.hub.stop();
        rmSync(dataDir, { recursive: true, force: true });
    }
});

import { loadConfig } from '../src/config.ts';
import { launchDocument } from '../src/configurations/store.ts';
import { writeFileSync } from 'node:fs';

test('startup discovers configuration inside the selected persistent root', () => {
    const dataDir = mkdtempSync(join(tmpdir(), 'simplex-config-'));
    try {
        const file = join(dataDir, 'hub.config.jsonc');
        writeFileSync(file, '{ "listen": { "port": 9123 }, "worker": { "threads": 4 } }');
        const loaded = loadConfig({ overrides: { dataDir } });
        assert.equal(loaded.file, file);
        assert.equal(loaded.config.listen.port, 9123);
        assert.equal(loaded.config.worker.threads, 4);
        assert.equal(loaded.config.dataDir, dataDir);
    } finally { rmSync(dataDir, { recursive: true, force: true }); }
});

test('deployment templates copy existing launcher and mock choices; local defaults stay independent', () => {
    const dataDir = mkdtempSync(join(tmpdir(), 'simplex-config-'));
    try {
        const config = defaultConfig();
        config.dataDir = dataDir;
        config.mock.enabled = true;
        config.worker.hubRemoteCall = false;
        config.worker.threads = 6;
        config.worker.connectHost = '172.17.0.1';
        config.launcher = { ...config.launcher, kind: 'command', command: ['wrapper', '{config}'] };
        const store = new ConfigurationStore(config);
        const launch = launchDocument(store.template('launch', 'deployment'), config);
        assert.deepEqual(launch.launcher.command, ['wrapper', '{config}']);
        assert.equal(launch.worker.threads, 6);
        const worker = parseDocument(store.template('worker', 'deployment')).toJS();
        assert.equal(worker.driver_model, 'mock');
        assert.equal(worker.hub_remote_call, undefined);
        assert.equal(worker.modality_assist_model, undefined);
        store.validate('worker', store.template('worker', 'deployment'));
        const local = launchDocument('{"launcher":{"kind":"command","command":["simplex","run"]}}', config);
        assert.equal(local.worker.threads, 1);
        assert.equal(local.worker.connectHost, '');
        assert.throws(() => launchDocument('{"launcher":{"kind":"simplex-worker"}}', config), /worker.bin/);
        assert.throws(() => store.validate('worker', store.template('worker').replace('state: state', 'state: ../outside')), /relative child/);
    } finally { rmSync(dataDir, { recursive: true, force: true }); }
});
