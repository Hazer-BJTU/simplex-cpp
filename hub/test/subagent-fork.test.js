/** Startup configuration relocation preserves meanings rather than library defaults. */
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { it } from 'node:test';
import { parse, stringify } from 'yaml';
import { testConfig } from './helpers/hub.js';
import { Session } from '../src/state/registry.ts';
import { captureStartup, cleanFork, supportedLaunch } from '../src/subagents/fork.ts';
import { sessionDir } from '../src/launch/config-render.ts';
import { launchDocument } from '../src/configurations/store.ts';

it('preserves all startup roles/options, relative external paths and launch environment in a clean fork', t => {
    const dataDir = mkdtempSync(join(tmpdir(), 'simplex-fork-paths-'));
    t.after(() => rmSync(dataDir, { recursive: true, force: true }));
    const config = testConfig({ dataDir });
    const parent = new Session({ id: 'parent' });
    parent.process = { state: 'running' };
    parent.lifecycleId = 'parent-incarnation';
    const child = new Session({ id: 'subagent-12345678-1234-4123-8123-123456789abc' });
    const source = join(sessionDir(config, parent.id), 'config');
    const settings = {
        worker: { environment: { workspace: '../../shared-workspace', software: ['bash'] } },
        plugins: { providers: { directories: ['../../providers'] }, extensions: {
            tools: { directories: ['../../tool-plugins'], enable: [{ name: 'tools', schema_directory: '../schemas' }] },
            loop_hooks: { directories: ['../../hooks'], enable: [{ name: 'hook', config_file: '../hook.yaml' }] },
        } },
        providers: { chosen: { plugin: 'qwen', model: 'qwen-vl', endpoint: { base_url: 'https://provider.invalid/v1' },
            generation: { temperature: 0.7, reasoning_effort: 'high' } } },
        models: { driver_model: 'chosen', modality_assist_model: 'chosen' },
        prompts: { system: 'general_agent.yaml' },
        persistence: { directory: '/parent-only', restore: 'required', state: 'custom-state', memory: 'custom-memory' },
        security: { confirmation: { endpoint: 'ws://old.invalid/confirm' } },
    };
    const launch = { launcher: { kind: 'simplex-worker', args: [], cwd: '/shared-launch-directory' },
        worker: { bin: '/absolute/simplex_worker', threads: 3 }, env: { CUSTOM_SETTING: 'preserved' } };
    captureStartup(config, parent, stringify(settings), launch);
    cleanFork(config, parent, child);
    const root = sessionDir(config, child.id);
    const saved = parse(readFileSync(join(root, 'config/config.yaml'), 'utf8'));
    assert.deepEqual(saved.providers, settings.providers);
    assert.deepEqual(saved.models, settings.models);
    assert.deepEqual(saved.prompts, settings.prompts);
    assert.equal(saved.worker.environment.workspace, resolve(source, '../../shared-workspace'));
    assert.equal(saved.plugins.providers.directories[0], resolve(source, '../../providers'));
    assert.equal(saved.plugins.extensions.tools.enable[0].schema_directory, resolve(source, '../schemas'));
    assert.equal(saved.plugins.extensions.loop_hooks.enable[0].config_file, resolve(source, '../hook.yaml'));
    assert.deepEqual(saved.persistence, { directory: root, restore: 'if_present', state: 'custom-state', memory: 'custom-memory' });
    assert.equal(saved.security.confirmation.endpoint, '{{hub.confirm_endpoint}}');
    assert.equal(saved.hub_remote_call.endpoint, '{{hub.tools_endpoint}}');
    assert.deepEqual(JSON.parse(readFileSync(join(root, 'config/launch.jsonc'), 'utf8')), launchDocument(JSON.stringify(launch), config));
});

it('accepts the foreground container boundary and rejects detached/config-overriding launchers', () => {
    const command = ['docker', 'run', '--rm', '--init', '--name', 'worker-{session}',
        '-v', '{session_dir}:{session_dir}', 'image', '--config', '{config}', '--session', '{session}'];
    const launch = { launcher: { kind: 'command', command, args: [] } };
    assert.equal(supportedLaunch(launch), true);
    assert.equal(supportedLaunch({ launcher: { ...launch.launcher, command: [...command, '--detach'] } }), false);
    assert.equal(supportedLaunch({ launcher: { ...launch.launcher, pidFile: 'worker.pid' } }), false);
    assert.equal(supportedLaunch({ launcher: { kind: 'simplex-worker', args: ['--session=shared'] } }), false);
});
