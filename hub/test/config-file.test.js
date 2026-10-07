/** Durable configuration reuse and the single session directory contract. */
import assert from 'node:assert/strict';
import { chmodSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, describe, it } from 'node:test';
import { parse, stringify } from 'yaml';
import { prepareSessionConfig, persistenceChild, sessionStateDirectory } from '../src/launch/config-file.ts';
import { sessionDir, workerConfigPath } from '../src/launch/config-render.ts';
import { testConfig } from './helpers/hub.js';

const roots = [];
afterEach(() => { for (const root of roots.splice(0)) rmSync(root, { recursive: true, force: true }); });
function options(sessionId = 'demo') {
    const root = mkdtempSync(join(tmpdir(), 'simplex-config-reuse-'));
    roots.push(root);
    return {
        config: testConfig({ dataDir: root, worker: { hubRemoteCall: true } }), sessionId, rawSpec: {},
        endpoints: { events: 'ws://old/events?token=old', confirm: 'ws://old/confirm?token=old', tools: 'ws://old/tools?token=old' },
    };
}

describe('session configuration files', () => {
    it('uses a direct session root and isolated state and memory subdirectories', () => {
        const input = options();
        const first = prepareSessionConfig(input).document;
        const second = prepareSessionConfig({ ...input, sessionId: 'other' }).document;
        assert.equal(first.persistence.directory, join(input.config.dataDir, 'sessions', 'demo'));
        assert.equal(first.persistence.state, 'state');
        assert.equal(first.persistence.memory, 'memory');
        assert.notEqual(first.persistence.directory, second.persistence.directory);
        assert.equal(workerConfigPath(input.config, 'demo'), join(first.persistence.directory, 'config/config.yaml'));
        assert.equal(sessionStateDirectory(input.config, 'demo'), join(first.persistence.directory, 'state'));
    });

    it('preserves operator YAML, comments and credentials while refreshing live connections', () => {
        const input = options();
        const initial = prepareSessionConfig(input).document;
        const path = workerConfigPath(input.config, input.sessionId);
        initial.providers.deepseek.model = 'operator-model';
        initial.providers.deepseek.endpoint.auth.api_key = '${OPERATOR_KEY}';
        initial.worker.max_exchanges = 47;
        initial.worker.auto_compact_threshold = 123456;
        initial.worker.max_auto_compactions = 3;
        initial.worker.auto_compact_prompt_file = 'prompts/custom-handoff.yaml';
        initial.hub_remote_call.timeout_ms = 750;
        initial.persistence.state = 'custom/snapshots';
        initial.persistence.memory = 'custom/archives';
        initial.future_extension = { untouched: ['one', 'two'] };
        writeFileSync(path, '# Operator comment\n' + stringify(initial));
        input.config.worker.maxExchanges = 999;
        input.config.providerProfiles = { replacement: { plugin: 'deepseek', model: 'new-default' } };
        input.rawSpec = { provider: 'deepseek', model: 'ignored', threads: 3 };
        input.endpoints = { events: 'ws://new/events?token=new', confirm: 'ws://new/confirm?token=new', tools: 'ws://new/tools?token=new' };
        const reused = prepareSessionConfig(input);
        assert.equal(reused.spec.threads, 3);
        assert.equal(reused.spec.autoCompactThreshold, 123456);
        assert.equal(reused.spec.maxAutoCompactions, 3);
        assert.equal(reused.document.worker.auto_compact_prompt_file, 'prompts/custom-handoff.yaml');
        assert.equal(reused.spec.provider, 'deepseek');
        assert.equal(reused.spec.model, 'operator-model');
        assert.equal(reused.spec.modalityAssistProvider, 'modality_assist');
        assert.deepEqual(reused.document, {
            ...initial,
            client: { ...initial.client, endpoint: input.endpoints.events },
            hub_remote_call: { ...initial.hub_remote_call, endpoint: input.endpoints.tools },
            security: { confirmation: { ...initial.security.confirmation, endpoint: input.endpoints.confirm } },
        });
        assert.match(readFileSync(path, 'utf8'), /# Operator comment/);
        assert.equal(sessionStateDirectory(input.config, input.sessionId),
            join(sessionDir(input.config, input.sessionId), 'custom/snapshots'));
        assert.deepEqual(readdirSync(join(sessionDir(input.config, input.sessionId), 'config')), ['config.yaml']);
    });

    it('defaults the assistant only on first creation and preserves operator choices', () => {
        const input = options();
        const initial = prepareSessionConfig(input).document;
        assert.equal(initial.modality_assist_model, 'modality_assist');
        const path = workerConfigPath(input.config, input.sessionId);
        initial.providers.vision = structuredClone(initial.providers.modality_assist);
        initial.modality_assist_model = 'vision';
        writeFileSync(path, stringify(initial));
        input.rawSpec = { provider: 'deepseek', modalityAssistProvider: 'deepseek' };
        const retargeted = prepareSessionConfig(input);
        assert.equal(retargeted.document.modality_assist_model, 'vision');
        assert.equal(retargeted.spec.modalityAssistProvider, 'vision');

        delete initial.modality_assist_model;
        writeFileSync(path, stringify(initial));
        const disabled = prepareSessionConfig(input);
        assert.equal(Object.hasOwn(disabled.document, 'modality_assist_model'), false);
        assert.equal(disabled.spec.modalityAssistProvider, null);
        assert.equal(prepareSessionConfig({
            ...input, rawSpec: disabled.spec,
        }).spec.modalityAssistProvider, null);
    });

    it('refreshes the dynamic mock URL without replacing provider settings', () => {
        const input = options();
        input.rawSpec = { provider: 'mock' };
        input.mock = { baseUrl: 'http://localhost:10001' };
        prepareSessionConfig(input);
        input.mock = { baseUrl: 'http://localhost:10002' };
        const result = prepareSessionConfig(input).document;
        assert.equal(result.providers.mock.endpoint.base_url, input.mock.baseUrl);
        assert.equal(result.driver_model, 'mock');
    });

    it('preserves existing access permissions across a configuration refresh', () => {
        const input = options();
        prepareSessionConfig(input);
        const path = workerConfigPath(input.config, input.sessionId);
        assert.equal(statSync(path).mode & 0o777, 0o600);
        chmodSync(path, 0o640);
        const before = statSync(path);
        prepareSessionConfig(input);
        const after = statSync(path);
        assert.equal(after.mode & 0o777, 0o640);
        assert.equal(after.uid, before.uid);
        assert.equal(after.gid, before.gid);
    });

    it('rejects invalid files and child paths without replacing saved bytes', () => {
        const input = options();
        prepareSessionConfig(input);
        const path = workerConfigPath(input.config, input.sessionId);
        for (const invalid of ['[broken', 'null', 'client: wrong', 'hub_remote_call: null',
            'persistence:\n  state: ../escape', 'persistence:\n  memory: null',
            'modality_assist_model: missing', 'modality_assist_model: null']) {
            writeFileSync(path, invalid);
            assert.throws(() => prepareSessionConfig(input));
            assert.equal(readFileSync(path, 'utf8'), invalid);
        }
        for (const invalid of ['', '/absolute', '../escape', 'nested/../../escape', null, 3, 'x\0y']) {
            assert.throws(() => persistenceChild('/root', invalid, 'state'));
        }
    });

    it('reasserts the hub-owned root rather than moving the session on restart', () => {
        const input = options();
        const result = prepareSessionConfig(input).document;
        const path = workerConfigPath(input.config, input.sessionId);
        result.persistence.directory = '/somewhere/else';
        writeFileSync(path, stringify(result));
        prepareSessionConfig(input);
        assert.equal(parse(readFileSync(path, 'utf8')).persistence.directory,
            sessionDir(input.config, input.sessionId));
    });
});

it('preserves disabled remote calls on restart even if hub defaults enable them', () => {
    const input = options();
    const path = workerConfigPath(input.config, input.sessionId);
    const original = prepareSessionConfig(input).document;
    delete original.hub_remote_call;
    writeFileSync(path, stringify(original));
    const restarted = prepareSessionConfig(input).document;
    assert.equal(Object.hasOwn(restarted, 'hub_remote_call'), false);
});

it('omits remote calls by default and preserves enabled settings when defaults change', () => {
    const input = options();
    input.config.worker.hubRemoteCall = false;
    assert.equal(Object.hasOwn(prepareSessionConfig(input).document, 'hub_remote_call'), false);
    const path = workerConfigPath(input.config, input.sessionId);
    const document = parse(readFileSync(path, 'utf8'));
    document.hub_remote_call = { endpoint: 'ws://old/tools', timeout_ms: 800, future_option: 'preserved' };
    writeFileSync(path, stringify(document));
    input.endpoints.tools = 'ws://new-host:9900/agent/demo/tools?token=rotated';
    const restarted = prepareSessionConfig(input).document;
    assert.deepEqual(restarted.hub_remote_call, {
        endpoint: input.endpoints.tools, timeout_ms: 800, future_option: 'preserved',
    });
});
