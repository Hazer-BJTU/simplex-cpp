import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { defaultConfig } from '../src/config.ts';
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
