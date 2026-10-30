import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { defaultConfig, mergeConfig, parseConfigText, validateConfig } from '../src/config.ts';
import { normalizeSpec } from '../src/launch/spec.ts';
import { renderWorkerConfig } from '../src/launch/config-render.ts';

const example = parseConfigText(readFileSync(new URL('../hub.config.example.jsonc', import.meta.url), 'utf8'), 'Hub example');
const legacy = parseConfigText(readFileSync(new URL('../hub.config.legacy.example.jsonc', import.meta.url), 'utf8'), 'Legacy example');
const readme = readFileSync(new URL('../README.md', import.meta.url), 'utf8');

/** Provider profile contents are plugin-owned maps, not fixed Hub config keys. */
function keyPaths(value, prefix = '') {
    if (prefix === 'providerProfiles' || !value || typeof value !== 'object' || Array.isArray(value)) return [prefix];
    return Object.entries(value).flatMap(([key, child]) => keyPaths(child, prefix ? `${prefix}.${key}` : key));
}

function missingKeys(defaults, sample, markdown) {
    const documented = new Set(keyPaths(sample));
    for (const line of markdown.split('\n')) {
        if (!line.startsWith('|')) continue;
        const keyCell = line.split('|')[1];
        for (const match of keyCell.matchAll(/`([^`]+)`/g)) documented.add(match[1]);
    }
    return keyPaths(defaults).filter(key => !documented.has(key));
}

test('every fixed Hub default key is discoverable in the legacy reference or README table', () => {
    assert.deepEqual(missingKeys(defaultConfig(), legacy, readme), []);
    assert.doesNotThrow(() => validateConfig(mergeConfig(defaultConfig(), example)));
    assert.doesNotThrow(() => validateConfig(mergeConfig(defaultConfig(), legacy)));
    for (const key of ['worker', 'launcher', 'providerProfiles', 'mock']) assert.equal(Object.hasOwn(example, key), false);
});

test('config documentation coverage detects new nested leaves rather than accepting a broad parent row', () => {
    const changed = defaultConfig();
    changed.worker.persistence.futurePolicy = true;
    changed.limits.futureBytes = 100;
    assert.deepEqual(missingKeys(changed, legacy, readme), ['worker.persistence.futurePolicy', 'limits.futureBytes']);
});

test('auto-compact defaults remain documented in the optional API-launch references', () => {
    const defaults = defaultConfig();
    for (const key of ['autoCompactThreshold', 'maxAutoCompactions']) {
        assert.equal(legacy.worker[key], defaults.worker[key]);
        assert.ok(readme.includes(`\`worker.${key}\``));
        for (const file of ['configurations.md', 'hub-protocol.md']) {
            const text = readFileSync(new URL(`../../docs/hub/${file}`, import.meta.url), 'utf8');
            assert.ok(text.includes(key), `${file} must describe ${key}`);
        }
    }
});

test('documented Hub auto-compact policy maps to worker fields and session overrides', () => {
    const config = defaultConfig();
    config.worker.autoCompactThreshold = 128000;
    config.worker.maxAutoCompactions = 3;
    const endpoints = { events: 'ws://localhost/events', confirm: 'ws://localhost/confirm', tools: 'ws://localhost/tools' };
    const initial = renderWorkerConfig({ config, sessionId: 'policy', spec: normalizeSpec(config), endpoints });
    assert.equal(initial.worker.auto_compact_threshold, 128000);
    assert.equal(initial.worker.max_auto_compactions, 3);
    const overridden = renderWorkerConfig({ config, sessionId: 'policy',
        spec: normalizeSpec(config, { autoCompactThreshold: 0, maxAutoCompactions: 7 }), endpoints });
    assert.equal(overridden.worker.auto_compact_threshold, 0);
    assert.equal(overridden.worker.max_auto_compactions, 7);
});

test('retained Docker examples forward the bundled worker credential before container creation', () => {
    const template = readFileSync(new URL('../schemas/worker.yaml', import.meta.url), 'utf8');
    const credential = template.match(/api_key:\s*["']?\$\{([A-Z_]+)\}/)?.[1];
    assert.equal(credential, 'MODEL_API_KEY');
    const tutorial = readFileSync(new URL('../../docs/deployment/docker-worker.md', import.meta.url), 'utf8');
    for (const text of [tutorial, readme]) {
        const shell = [...text.matchAll(/```sh\n([\s\S]*?)```/g)].map(match => match[1]).join('\n');
        const commands = shell.match(/docker run -d --name simplex-worker[^`]+?simplex-worker:latest/g);
        assert.ok(commands?.length);
        for (const command of commands) assert.ok(command.includes(`-e ${credential}`));
        assert.ok(shell.indexOf(`export ${credential}=`) < shell.indexOf('docker run -d --name simplex-worker'));
        assert.ok(text.includes(`test -n "$${credential}"`));
        assert.ok(text.includes(`api_key: \${${credential}}`));
        assert.ok(!text.includes('DEEPSEEK_API_KEY'));
    }
});
