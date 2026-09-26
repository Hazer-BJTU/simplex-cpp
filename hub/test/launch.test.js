/**
 * @file launch pipeline: spec defaults, generated worker configuration, and the
 * process invocation each launcher kind builds.
 */
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { ConfigError } from '../src/config.js';
import { buildCommandInvocation, expandTemplate } from '../src/launch/command.js';
import { persistenceRoot, renderSessionConfig, sessionDir } from '../src/launch/config-render.js';
import { createLauncher } from '../src/launch/launcher.js';
import { buildSimplexWorkerInvocation } from '../src/launch/simplex-worker.js';
import { normalizeSpec } from '../src/launch/spec.js';
import { createLogger } from '../src/log.ts';
import { testConfig } from './helpers/hub.js';

const log = createLogger({ level: 'silent' });
const endpoints = {
    events: 'ws://127.0.0.1:8800/agent/demo/events?token=t',
    confirm: 'ws://127.0.0.1:8800/agent/demo/confirm?token=t',
};

describe('normalizeSpec', () => {
    it('applies hub defaults', () => {
        const config = testConfig();
        const spec = normalizeSpec(config, {});
        assert.equal(spec.provider, 'deepseek');
        assert.equal(spec.threads, 1);
        assert.equal(spec.maxExchanges, 512);
        assert.equal(spec.persistence.enabled, true);
        assert.equal(spec.restore, 'if_present');
        assert.equal(spec.systemPromptFile, join(config.worker.promptsDir, 'coding_agent.yaml'));
    });

    it('selects a prompt file by name or by absolute path', () => {
        const config = testConfig();
        assert.equal(
            normalizeSpec(config, { systemPromptFile: 'other.yaml' }).systemPromptFile,
            join(config.worker.promptsDir, 'other.yaml'));
        assert.equal(
            normalizeSpec(config, { systemPromptFile: '/tmp/agent.yaml' }).systemPromptFile,
            '/tmp/agent.yaml');
    });

    it('honours per-session overrides', () => {
        const config = testConfig();
        const spec = normalizeSpec(config, {
            provider: 'mock',
            model: 'fake-model',
            threads: 4,
            persistence: { readable: true },
            restore: 'never',
            workspace: '/srv/work',
            software: ['python 3.12'],
            env: { EXTRA: '1' },
            extraArgs: ['--verbose'],
        });
        assert.equal(spec.provider, 'mock');
        assert.equal(spec.model, 'fake-model');
        assert.equal(spec.threads, 4);
        assert.equal(spec.persistence.readable, true);
        assert.equal(spec.restore, 'never');
        assert.equal(spec.workspace, '/srv/work');
        assert.deepEqual(spec.software, ['python 3.12']);
        assert.deepEqual(spec.env, { EXTRA: '1' });
        assert.deepEqual(spec.extraArgs, ['--verbose']);
    });

    it('rejects an unknown provider, a bad thread count, and a bad restore policy', () => {
        const config = testConfig();
        assert.throws(() => normalizeSpec(config, { provider: 'nope' }), ConfigError);
        assert.throws(() => normalizeSpec(config, { threads: 0 }), /threads/);
        assert.throws(() => normalizeSpec(config, { restore: 'maybe' }), /restore/);
        assert.throws(() => normalizeSpec(config, { software: 'python' }), /software/);
    });
});

describe('renderSessionConfig', () => {
    it('renders a worker configuration with endpoints and persistence', () => {
        const config = testConfig();
        const { spec, document } = renderSessionConfig({
            config,
            sessionId: 'demo',
            rawSpec: { provider: 'mock' },
            endpoints,
        });
        assert.equal(spec.provider, 'mock');
        assert.equal(document.driver_model, 'mock');
        assert.equal(document.client.endpoint, endpoints.events);
        assert.equal(document.security.confirmation.endpoint, endpoints.confirm);
        assert.equal(document.security.confirmation.timeout_ms, config.worker.confirmationTimeoutMs);
        assert.equal(document.persistence.directory, persistenceRoot(config));
        assert.equal(document.persistence.restore, 'if_present');
        assert.equal(document.worker.max_exchanges, 512);
        assert.equal(document.worker.environment.workspace, '');
        assert.deepEqual(document.plugins.extensions.tools.enable, []);
    });

    it('keeps the profile intact and overrides only what the session asked for', () => {
        const config = testConfig();
        const { document } = renderSessionConfig({
            config,
            sessionId: 'demo',
            rawSpec: { provider: 'deepseek', model: 'deepseek-v4-pro' },
            endpoints,
        });
        const profile = document.providers.deepseek;
        assert.equal(profile.model, 'deepseek-v4-pro');
        assert.equal(profile.plugin, 'deepseek');
        assert.equal(profile.endpoint.auth.api_key, '${DEEPSEEK_API_KEY}');
        assert.deepEqual(profile.retry, { max_attempts: 3, initial_backoff_ms: 500, max_backoff_ms: 120000 });
        // The configuration must be free of resolved secrets.
        assert.equal(JSON.stringify(document).includes('sk-'), false);
    });

    it('points the mock profile at the resolved mock address', () => {
        const config = testConfig();
        const { document } = renderSessionConfig({
            config,
            sessionId: 'demo',
            rawSpec: { provider: 'mock' },
            endpoints,
            mock: { baseUrl: 'http://127.0.0.1:4321' },
        });
        assert.equal(document.providers.mock.endpoint.base_url, 'http://127.0.0.1:4321');
        assert.equal(document.providers.mock.endpoint.auth.scheme, 'none');
    });

    it('derives per-session directories from the data directory', () => {
        const config = testConfig();
        assert.equal(sessionDir(config, 'demo'), join(config.dataDir, 'workers', 'demo'));
        assert.equal(persistenceRoot(config), join(config.dataDir, 'sessions'));
    });
});

describe('simplex-worker launcher', () => {
    it('builds the documented command line', () => {
        const config = testConfig({ launcher: { args: ['--extra'] } });
        const spec = normalizeSpec(config, { threads: 3, extraArgs: ['--session-extra'] });
        const invocation = buildSimplexWorkerInvocation({
            config,
            sessionId: 'demo',
            spec,
            configPath: '/tmp/config.yaml',
            sessionDir: '/tmp/session',
        });
        assert.equal(invocation.command, config.worker.bin);
        assert.deepEqual(invocation.args, [
            '--config', '/tmp/config.yaml',
            '--session', 'demo',
            '--threads', '3',
            '--extra',
            '--session-extra',
        ]);
        assert.equal(invocation.cwd, '/tmp/session');
        assert.equal(invocation.pidFile, null);
    });
});

describe('command launcher', () => {
    const config = testConfig({
        launcher: {
            kind: 'command',
            command: ['bash', 'run.sh', '{session}', '--config', '{config}'],
            args: ['--endpoint', '{endpoint}', '--token', '{token}'],
        },
    });

    it('expands every documented placeholder', () => {
        const spec = normalizeSpec(config, {});
        const invocation = buildCommandInvocation({
            config,
            sessionId: 'demo',
            spec,
            configPath: '/tmp/workers/demo/config.yaml',
            sessionDir: '/tmp/workers/demo',
            endpoints,
            token: 'sekret',
        });
        assert.equal(invocation.command, 'bash');
        assert.deepEqual(invocation.args, [
            'run.sh', 'demo',
            '--config', '/tmp/workers/demo/config.yaml',
            '--endpoint', endpoints.events,
            '--token', 'sekret',
        ]);
        assert.equal(invocation.cwd, '/tmp/workers/demo');
        // `command` may daemonize, so the launcher is treated as such.
        assert.equal(createLauncher({ config, log }).mayDaemonize, true);
    });

    it('rejects an unknown placeholder instead of passing it through', () => {
        assert.throws(() => expandTemplate('{nope}', { session: 'x' }), /unknown launcher placeholder/);
    });

    it('uses a configured working directory when given', () => {
        const withCwd = testConfig({ launcher: { kind: 'command', command: ['true'], cwd: '/srv' } });
        const invocation = buildCommandInvocation({
            config: withCwd,
            sessionId: 'demo',
            spec: normalizeSpec(withCwd, {}),
            configPath: '/tmp/config.yaml',
            sessionDir: '/tmp/session',
            endpoints,
            token: 't',
        });
        assert.equal(invocation.cwd, '/srv');
    });
});

describe('createLauncher', () => {
    it('reports the configured kind', () => {
        assert.equal(createLauncher({ config: testConfig(), log }).kind, 'simplex-worker');
        assert.equal(
            createLauncher({
                config: testConfig({ launcher: { kind: 'command', command: ['true'] } }),
                log,
            }).kind,
            'command');
    });

    it('reports whether the launcher owns configuration', () => {
        const owned = testConfig({ launcher: { config: 'launcher' } });
        assert.equal(createLauncher({ config: owned, log }).ownsConfig, true);
    });
});
