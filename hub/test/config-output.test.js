/** Generated user documents omit compatibility scaffolding without changing restart behavior. */
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';
import { defaultConfig, loadConfig, startupConfigDocument, validateConfig } from '../src/config.ts';
import { ConfigurationStore, launchDocument, serializeLaunchDocument } from '../src/configurations/store.ts';
import { resolvedStartupLaunch } from '../src/subagents/fork.ts';
import { HubState } from '../src/state/persist.ts';
import { createLogger } from '../src/log.ts';

function directory(t) {
    const root = mkdtempSync(join(tmpdir(), 'simplex-config-output-'));
    t.after(() => rmSync(root, { recursive: true, force: true }));
    return root;
}

for (const customized of [false, true]) {
    test(`first-run output reloads with identical effective settings (${customized ? 'custom' : 'default'})`, t => {
        const dataDir = directory(t);
        const overrides = { dataDir, ...(customized ? {
            worker: { bin: './chosen/simplex_worker', threads: 3, autoCompactThreshold: 24000 },
            launcher: { kind: 'command', command: ['wrapper', '{config}'], cwd: '/tmp' },
            mock: { enabled: true, slowMs: 22 },
            providerProfiles: { selected: { plugin: 'qwen', model: 'operator-model' } },
            limits: { logLines: 91 }, panel: { token: 'operator-token' },
        } : {}) };
        const { config } = loadConfig({ overrides, cwd: dataDir });
        const before = structuredClone(config);
        const document = startupConfigDocument(config);
        assert.deepEqual(config, before, 'projection must not mutate runtime settings');
        if (!customized) {
            assert.deepEqual(Object.keys(document).sort(), ['dataDir', 'listen', 'panel', 'toolRequests']);
        } else {
            assert.equal(document.worker.threads, 3);
            assert.equal(document.mock.enabled, true);
            assert.equal(document.worker.bin, join(dataDir, 'chosen/simplex_worker'));
        }
        writeFileSync(join(dataDir, 'hub.config.jsonc'), JSON.stringify(document));
        assert.deepEqual(loadConfig({ overrides: { dataDir } }).config, config);
    });
}

test('retired Hub fields are accepted and dropped; unknown automation settings still fail', t => {
    const dataDir = directory(t);
    const legacy = defaultConfig();
    legacy.dataDir = dataDir;
    legacy.worker.persistence.readable = true;
    legacy.launcher.dockerManagementEnv = { OLD_FLAG: 'unused' };
    const config = validateConfig(legacy);
    assert.equal(Object.hasOwn(config.worker.persistence, 'readable'), false);
    assert.equal(Object.hasOwn(config.launcher, 'dockerManagementEnv'), false);
    assert.equal(JSON.stringify(startupConfigDocument(config)).includes('readable'), false);
    config.launcher.misspelledSetting = true;
    assert.throws(() => validateConfig(config), /unknown configuration/);
});

for (const source of ['default', 'managed', 'docker']) {
    test(`managed ${source} snapshots reload without legacy launch fields`, t => {
        const config = { ...defaultConfig(), dataDir: directory(t) };
        const store = new ConfigurationStore(config);
        const launch = launchDocument(store.template('launch', source), config);
        launch.worker.args = ['--custom'];
        const snapshot = resolvedStartupLaunch({ ...config, worker: { ...config.worker, ...launch.worker } },
            launch, { OPERATOR_FLAG: 'retained' });
        const before = structuredClone(snapshot);
        const text = serializeLaunchDocument(snapshot);
        const raw = JSON.parse(text);
        assert.equal(Object.hasOwn(raw, 'launcher'), false);
        assert.equal(Object.hasOwn(raw.worker, 'bin'), false);
        assert.deepEqual(snapshot, before);
        const restored = launchDocument(text, config);
        assert.deepEqual(restored.management, snapshot.management);
        assert.deepEqual(restored.worker.args, ['--custom']);
        assert.deepEqual(restored.env, { OPERATOR_FLAG: 'retained' });
        assert.equal(serializeLaunchDocument(restored), text, 'repeated snapshots must remain stable');
    });
}

test('legacy launch serialization preserves commands and strips only the retired Docker setting', t => {
    const config = { ...defaultConfig(), dataDir: directory(t) };
    const legacy = { launcher: { kind: 'command', command: ['wrapper', '{config}'],
        dockerManagementEnv: { OBSOLETE: 'ignored' } }, env: { IMPORTANT: 'kept' } };
    const launch = launchDocument(JSON.stringify(legacy), config);
    const raw = JSON.parse(serializeLaunchDocument(launch));
    assert.deepEqual(raw.launcher.command, legacy.launcher.command);
    assert.deepEqual(raw.env, legacy.env);
    assert.equal(Object.hasOwn(raw.launcher, 'dockerManagementEnv'), false);
});

test('saving an old stopped session removes retired metadata without losing ownership or selectors', t => {
    const dataDir = directory(t);
    const state = new HubState({ config: { dataDir }, log: createLogger({ level: 'silent' }) });
    const session = { id: 'old', token: 'retained-token', lifecycleId: 'retained-lifecycle', createdAt: 'then',
        spec: { launchConfig: 'local', workerConfig: 'chosen',
            persistence: { readable: true, enabled: true, backend: 'json' }, future: { keep: true } },
        process: { pid: 12345, pidStartTime: 'start-time', startedAt: 'then',
            command: 'simplex', args: ['run'], cwd: '/tmp', state: 'stopped' } };
    const before = structuredClone(session);
    assert.equal(state.save([session]), true);
    const saved = JSON.parse(readFileSync(join(dataDir, 'hub.json'), 'utf8')).sessions[0];
    assert.deepEqual(session, before);
    assert.deepEqual(saved.spec.persistence, { enabled: true, backend: 'json' });
    assert.equal(saved.spec.launchConfig, 'local');
    assert.equal(saved.spec.workerConfig, 'chosen');
    assert.deepEqual(saved.spec.future, { keep: true });
    assert.equal(saved.token, session.token);
    assert.equal(saved.lifecycle_id, session.lifecycleId);
    assert.equal(saved.process.pid_start_time, 'start-time');
    assert.deepEqual(state.load().sessions[0], saved);
});
