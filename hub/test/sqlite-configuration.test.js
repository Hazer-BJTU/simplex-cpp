import test from 'node:test';
import assert from 'node:assert/strict';
import { defaultConfig } from '../src/config.ts';
import { normalizeSpec } from '../src/launch/spec.ts';
import { renderWorkerConfig } from '../src/launch/config-render.ts';
import { applyConfigurationSpec } from '../src/launch/config-file.ts';

const endpoints = { events: 'ws://localhost/events', confirm: 'ws://localhost/confirm', tools: 'ws://localhost/tools' };
test('new configurations select SQLite explicitly without the conflicting legacy format alias', () => {
    const config = defaultConfig();
    const spec = normalizeSpec(config);
    const document = renderWorkerConfig({ config, sessionId: 'new', spec, endpoints });
    assert.equal(document.persistence.backend, 'sqlite');
    assert.equal(Object.hasOwn(document.persistence, 'format'), false);
    assert.equal(Object.hasOwn(document.persistence, 'readable'), false);
    const json = renderWorkerConfig({ config, sessionId: 'json', endpoints,
        spec: normalizeSpec(config, { persistence: { backend: 'json' } }) });
    assert.equal(json.persistence.backend, 'json');
    assert.throws(() => normalizeSpec(config, { persistence: { backend: 'unknown' } }), /backend/);
});
test('saved backend policy survives configuration refresh and does not inherit new defaults', () => {
    const config = defaultConfig();
    for (const [storage, expected] of [
        [{ backend: 'json' }, 'json'], [{ backend: 'sqlite' }, 'sqlite'],
        [{ format: 'json' }, 'json'], [{}, undefined],
    ]) {
        const spec = normalizeSpec(config);
        const original = structuredClone(storage);
        applyConfigurationSpec(spec, { persistence: storage });
        assert.equal(spec.persistence.backend, expected);
        assert.deepEqual(storage, original);
    }
});

test('saved conflicting or unsupported backend/format declarations fail explicitly', () => {
    for (const persistence of [
        { backend: 'sqlite', format: 'json' }, { format: 'markdown' }, { backend: 'unknown' },
    ]) {
        assert.throws(() => applyConfigurationSpec(normalizeSpec(defaultConfig()), { persistence }),
            /backend|format/);
    }
});
