/** Durable configuration reuse and the single session directory contract. */
import assert from 'node:assert/strict';
import { chmodSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, describe, it } from 'node:test';
import { parse, stringify } from 'yaml';
import { missingPromptDiagnostic, prepareSessionConfig, persistenceChild, sessionStateDirectory }
    from '../src/launch/config-file.ts';
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

/** A known local installation, independent of any real worker on the test host. */
function currentInstallation(input) {
    const bin = join(input.config.dataDir, 'installation', 'bin');
    mkdirSync(join(bin, 'prompts'), { recursive: true });
    writeFileSync(join(bin, 'simplex_worker'), '');
    writeFileSync(join(bin, 'prompts/default.yaml'), 'default');
    input.config.worker.bin = join(bin, 'simplex_worker');
    return bin;
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

    it('migrates a saved legacy default prompt once and inherits the deployed default', () => {
        const input = options();
        currentInstallation(input);
        const path = workerConfigPath(input.config, input.sessionId);
        const saved = prepareSessionConfig(input).document;
        // Every Hub through v1.3.0 wrote this key into each configuration.
        saved.worker.system_prompt_file = 'prompts/coding_agent.yaml';
        writeFileSync(path, '# operator note\n' + stringify(saved));

        const migrated = prepareSessionConfig(input);
        assert.equal(migrated.migratedPrompt, 'prompts/coding_agent.yaml');
        assert.equal(migrated.spec.systemPromptFile, undefined);
        assert.equal(Object.hasOwn(migrated.document.worker, 'system_prompt_file'), false);
        const text = readFileSync(path, 'utf8');
        assert.match(text, /# operator note/);
        assert.equal(Object.hasOwn(parse(text).worker, 'system_prompt_file'), false);

        // The rewrite is persisted: a later start has nothing left to migrate.
        input.config.providerProfiles = { replacement: { plugin: 'deepseek', model: 'new-model' } };
        const restart = prepareSessionConfig(input);
        assert.equal(restart.migratedPrompt, undefined);
        assert.equal(restart.spec.systemPromptFile, undefined);
        assert.equal(restart.document.driver_model, saved.driver_model);
    });

    it('migrates the other historical name but never rewrites a custom prompt', () => {
        const input = options();
        currentInstallation(input);
        const path = workerConfigPath(input.config, input.sessionId);
        const saved = prepareSessionConfig(input).document;
        saved.worker.system_prompt_file = 'prompts/general_agent.yaml';
        writeFileSync(path, stringify(saved));
        const migrated = prepareSessionConfig(input);
        assert.equal(migrated.migratedPrompt, 'prompts/general_agent.yaml');
        assert.equal(Object.hasOwn(migrated.document.worker, 'system_prompt_file'), false);

        const custom = prepareSessionConfig(input).document;
        custom.worker.system_prompt_file = 'prompts/my_agent.yaml';
        writeFileSync(path, stringify(custom));
        const kept = prepareSessionConfig(input);
        assert.equal(kept.migratedPrompt, undefined);
        assert.equal(kept.spec.systemPromptFile, 'prompts/my_agent.yaml');
        assert.equal(parse(readFileSync(path, 'utf8')).worker.system_prompt_file, 'prompts/my_agent.yaml');
    });

    it('preserves a legacy selection when its file or symlink still exists', () => {
        const input = options();
        const bin = currentInstallation(input);
        const path = workerConfigPath(input.config, input.sessionId);
        const saved = prepareSessionConfig(input).document;
        saved.worker.system_prompt_file = 'prompts/coding_agent.yaml';
        writeFileSync(path, stringify(saved));
        const legacy = join(bin, saved.worker.system_prompt_file);
        writeFileSync(legacy, 'operator-customized legacy file');
        assert.equal(prepareSessionConfig(input).migratedPrompt, undefined);
        assert.equal(parse(readFileSync(path, 'utf8')).worker.system_prompt_file,
            saved.worker.system_prompt_file);
        rmSync(legacy);
        symlinkSync(join(bin, 'missing-custom-target.yaml'), legacy);
        assert.equal(prepareSessionConfig(input).migratedPrompt, undefined);
        assert.equal(parse(readFileSync(path, 'utf8')).worker.system_prompt_file,
            saved.worker.system_prompt_file);
    });

    it('preserves legacy names without a known replacement or direct local launcher', () => {
        const input = options();
        const bin = currentInstallation(input);
        const path = workerConfigPath(input.config, input.sessionId);
        const saved = prepareSessionConfig(input).document;
        saved.worker.system_prompt_file = 'prompts/general_agent.yaml';
        writeFileSync(path, stringify(saved));
        rmSync(join(bin, 'prompts/default.yaml'));
        assert.equal(prepareSessionConfig(input).migratedPrompt, undefined);
        // A directory is not a replacement prompt either.
        mkdirSync(join(bin, 'prompts/default.yaml'));
        assert.equal(prepareSessionConfig(input).migratedPrompt, undefined);
        rmSync(join(bin, 'prompts/default.yaml'), { recursive: true });
        writeFileSync(join(bin, 'prompts/default.yaml'), 'default');
        input.config.launcher.kind = 'command';
        assert.equal(prepareSessionConfig(input).migratedPrompt, undefined);
        input.config.launcher.kind = 'simplex-worker';
        input.config.worker.bin = 'simplex_worker';
        assert.equal(prepareSessionConfig(input).migratedPrompt, undefined);
        assert.equal(parse(readFileSync(path, 'utf8')).worker.system_prompt_file,
            saved.worker.system_prompt_file);
    });

    it('follows an executable symlink to the actual installation before migrating', () => {
        const input = options();
        const bin = currentInstallation(input);
        const link = join(input.config.dataDir, 'worker-link');
        symlinkSync(input.config.worker.bin, link);
        input.config.worker.bin = link;
        const saved = prepareSessionConfig(input).document;
        saved.worker.system_prompt_file = 'prompts/coding_agent.yaml';
        writeFileSync(workerConfigPath(input.config, input.sessionId), stringify(saved));
        assert.equal(prepareSessionConfig(input).migratedPrompt, 'prompts/coding_agent.yaml');
        assert.equal(readFileSync(join(bin, 'prompts/default.yaml'), 'utf8'), 'default');
    });
});

describe('missing prompt diagnostics', () => {
    it('names the missing file, the installation and both remedies', () => {
        const root = mkdtempSync(join(tmpdir(), 'simplex-prompt-diagnostic-'));
        roots.push(root);
        mkdirSync(join(root, 'bin', 'prompts'), { recursive: true });
        writeFileSync(join(root, 'bin', 'prompts', 'default.yaml'), 'base');
        writeFileSync(join(root, 'bin', 'simplex_worker'), '');
        const config = testConfig({ worker: { bin: join(root, 'bin', 'simplex_worker') } });
        const diagnostic = missingPromptDiagnostic({ systemPromptFile: 'prompts/absent.yaml' }, config);
        assert.match(diagnostic, /prompts\/absent\.yaml/);
        assert.match(diagnostic, /remove worker\.system_prompt_file/);
        assert.match(diagnostic, /prompts\/default\.yaml/);
        assert.ok(diagnostic.includes(join(root, 'bin', 'prompts', 'absent.yaml')));
        assert.equal(missingPromptDiagnostic({ systemPromptFile: 'prompts/default.yaml' }, config), null);
        assert.equal(missingPromptDiagnostic({}, config), null);
        // A binary resolved through PATH has no installation directory to inspect.
        assert.equal(missingPromptDiagnostic({ systemPromptFile: 'prompts/absent.yaml' },
            { ...config, worker: { ...config.worker, bin: 'simplex_worker' } }), null);
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
