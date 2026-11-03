/** Exercise prompt loading in real workers, not only archive layout validation.
 * CI supplies the current staged worker. SIMPLEX_HISTORICAL_WORKER_BIN optionally
 * adds an official older installation (e.g. v1.3.0 with coding_agent.yaml only).
 * No model request is sent and no external endpoint is contacted.
 */
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import { WebSocketServer } from 'ws';
import { parse } from 'yaml';
import { defaultConfig } from '../../src/config.ts';
import { normalizeSpec } from '../../src/launch/spec.ts';
import { renderWorkerConfig, workerConfigPath } from '../../src/launch/config-render.ts';
import { prepareSessionConfig } from '../../src/launch/config-file.ts';

const current = process.env.SIMPLEX_WORKER_BIN
    ?? fileURLToPath(new URL('../../../build/bin/simplex_worker', import.meta.url));
const historical = process.env.SIMPLEX_HISTORICAL_WORKER_BIN;
for (const [label, binary] of [['current', current], ['historical', historical]]) {
    test(`${label} worker inherits its prompt for generated and template configs`, {
        skip: !binary || (label === 'current' && !process.env.SIMPLEX_WORKER_BIN
            && !existsSync(binary) && process.env.SIMPLEX_E2E_REQUIRED !== '1'),
        timeout: 60000,
    }, async t => {
        assert.ok(existsSync(binary), `Missing ${label} worker: ${binary}`);
        if (label === 'historical') {
            assert.ok(existsSync(join(dirname(binary), 'prompts/coding_agent.yaml')));
            assert.equal(existsSync(join(dirname(binary), 'prompts/default.yaml')), false);
        } else {
            assert.ok(existsSync(join(dirname(binary), 'prompts/default.yaml')));
        }
        const root = mkdtempSync(join(tmpdir(), 'simplex-prompt-startup-'));
        t.after(() => rmSync(root, { recursive: true, force: true }));
        const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
        await once(server, 'listening');
        t.after(async () => {
            for (const client of server.clients) client.terminate();
            await new Promise(resolve => server.close(resolve));
        });
        const endpoint = `ws://127.0.0.1:${server.address().port}/events`;
        const config = defaultConfig();
        config.dataDir = root;
        config.providerProfiles = { fixture: {
            plugin: 'deepseek', model: 'deepseek-flash',
            endpoint: { base_url: 'http://127.0.0.1:1' },
        } };
        config.worker.persistence.enabled = false;
        for (const source of ['generated', 'template']) {
            // An installed operation prompt is also a valid custom system prompt.
            for (const selection of [undefined, 'prompts/operations/compact.yaml', 'prompts/missing-test.yaml']) {
                await t.test(`${source}: ${selection ?? 'worker default'}`, async () => {
                    const spec = normalizeSpec(config, selection === undefined ? {} : { systemPromptFile: selection });
                    const document = source === 'generated'
                        ? renderWorkerConfig({ config, sessionId: 'prompt-test', spec,
                            endpoints: { events: endpoint, confirm: endpoint, tools: endpoint } })
                        : parse(readFileSync(new URL('../../schemas/worker.yaml', import.meta.url), 'utf8'));
                    if (source === 'template') {
                        // Fill operator-required fields without altering prompt omission.
                        document.providers = config.providerProfiles;
                        document.driver_model = 'fixture';
                        document.client.endpoint = endpoint;
                        document.persistence = { enabled: false };
                        delete document.security;
                        if (selection !== undefined) document.worker.system_prompt_file = selection;
                    }
                    if (selection === undefined) assert.equal(Object.hasOwn(document.worker, 'system_prompt_file'), false);
                    else assert.equal(document.worker.system_prompt_file, selection);
                    const path = join(root, 'config.yaml');
                    writeFileSync(path, JSON.stringify(document));
                    const child = spawn(binary, ['--config', path, '--session', 'prompt-test'], { cwd: root, stdio: ['ignore', 'ignore', 'pipe'] });
                    let diagnostics = '';
                    child.stderr.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-65536); });
                    const exited = once(child, 'exit');
                    let connected = false;
                    const onConnection = () => { connected = true; child.kill('SIGTERM'); };
                    server.on('connection', onConnection);
                    const deadline = setTimeout(() => child.kill('SIGKILL'), 10000);
                    try {
                        const [code, signal] = await exited;
                        if (selection?.includes('missing-test')) {
                            assert.equal(connected, false);
                            assert.equal(code, 1, diagnostics);
                            assert.match(diagnostics, /missing-test\.yaml/);
                        } else {
                            assert.equal(connected, true, diagnostics);
                            assert.equal(signal, null, diagnostics);
                            assert.equal(code, 0, diagnostics);
                        }
                    } finally {
                        clearTimeout(deadline);
                        server.off('connection', onConnection);
                        if (child.exitCode === null && child.signalCode === null) {
                            child.kill('SIGKILL');
                            await exited;
                        }
                    }
                });
            }
        }
    });
}

/** A Hub through v1.3.0 wrote its own prompt file into every configuration. */
test('an old configuration that pins the removed default prompt starts the current worker', {
    skip: !process.env.SIMPLEX_WORKER_BIN && !existsSync(current)
        && process.env.SIMPLEX_E2E_REQUIRED !== '1',
    timeout: 60000,
}, async t => {
    assert.ok(existsSync(current), `Missing current worker: ${current}`);
    const root = mkdtempSync(join(tmpdir(), 'simplex-prompt-migration-'));
    t.after(() => rmSync(root, { recursive: true, force: true }));
    const server = new WebSocketServer({ host: '127.0.0.1', port: 0 });
    await once(server, 'listening');
    t.after(async () => {
        for (const client of server.clients) client.terminate();
        await new Promise(resolve => server.close(resolve));
    });
    const endpoint = `ws://127.0.0.1:${server.address().port}/events`;
    const config = defaultConfig();
    config.dataDir = root;
    config.worker.bin = current;
    config.providerProfiles = { fixture: {
        plugin: 'deepseek', model: 'deepseek-flash',
        endpoint: { base_url: 'http://127.0.0.1:1' },
    } };
    config.worker.persistence.enabled = false;
    const options = {
        config, sessionId: 'legacy-prompt', rawSpec: {},
        endpoints: { events: endpoint, confirm: endpoint, tools: endpoint },
    };
    for (const legacy of ['prompts/coding_agent.yaml', 'prompts/general_agent.yaml']) {
        await t.test(legacy, async () => {
            assert.equal(existsSync(join(dirname(current), legacy)), false,
                'migration requires an installation without the legacy file');
            // Write the file the way an older Hub did, then migrate it through
            // current startup preparation and verify the durable omission.
            const initial = prepareSessionConfig(options).document;
            initial.worker.system_prompt_file = legacy;
            const path = workerConfigPath(config, options.sessionId);
            writeFileSync(path, JSON.stringify(initial));
            const migrated = prepareSessionConfig(options);
            assert.equal(migrated.migratedPrompt, legacy);
            assert.equal(Object.hasOwn(parse(readFileSync(path, 'utf8')).worker, 'system_prompt_file'), false);
            assert.equal(prepareSessionConfig(options).migratedPrompt, undefined);

            const child = spawn(current, ['--config', path, '--session', options.sessionId],
                { cwd: root, stdio: ['ignore', 'ignore', 'pipe'] });
            let diagnostics = '';
            child.stderr.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-65536); });
            const exited = once(child, 'exit');
            let connected = false;
            const onConnection = () => { connected = true; child.kill('SIGTERM'); };
            server.on('connection', onConnection);
            const deadline = setTimeout(() => child.kill('SIGKILL'), 10000);
            try {
                const [code, signal] = await exited;
                assert.equal(connected, true, diagnostics);
                assert.equal(signal, null, diagnostics);
                assert.equal(code, 0, diagnostics);
            } finally {
                clearTimeout(deadline);
                server.off('connection', onConnection);
                if (child.exitCode === null && child.signalCode === null) {
                    child.kill('SIGKILL');
                    await exited;
                }
            }
        });
    }
});
