import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { defaultConfig, mergeConfig, parseConfigText, validateConfig } from '../src/config.ts';
import { normalizeSpec } from '../src/launch/spec.ts';
import { renderWorkerConfig } from '../src/launch/config-render.ts';

const example = parseConfigText(readFileSync(new URL('../hub.config.example.jsonc', import.meta.url), 'utf8'), 'Hub example');
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

test('every fixed Hub default key is discoverable in the startup example or README table', () => {
    assert.deepEqual(missingKeys(defaultConfig(), example, readme), []);
    assert.doesNotThrow(() => validateConfig(mergeConfig(defaultConfig(), example)));
});

test('config documentation coverage detects new nested leaves rather than accepting a broad parent row', () => {
    const changed = defaultConfig();
    changed.worker.persistence.futurePolicy = true;
    changed.limits.futureBytes = 100;
    assert.deepEqual(missingKeys(changed, example, readme), ['worker.persistence.futurePolicy', 'limits.futureBytes']);
});

test('auto-compact defaults are present in all Hub configuration references', () => {
    const defaults = defaultConfig();
    for (const key of ['autoCompactThreshold', 'maxAutoCompactions']) {
        assert.equal(example.worker[key], defaults.worker[key]);
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
