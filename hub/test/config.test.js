/**
 * @file configuration loading: comment tolerance, merging, path resolution,
 * and the validation rules that protect the approval boundary.
 */
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import {
    ConfigError,
    defaultConfig,
    loadConfig,
    mergeConfig,
    parseConfigText,
    stripJsonComments,
    validateConfig,
} from '../src/config.ts';

function tempDir() {
    return mkdtempSync(join(tmpdir(), 'simplex-hub-config-'));
}

describe('stripJsonComments', () => {
    it('removes line and block comments', () => {
        const text = '{\n  // a line\n  "a": 1, /* inline */ "b": 2\n}';
        assert.deepEqual(JSON.parse(stripJsonComments(text)), { a: 1, b: 2 });
    });

    it('leaves comment markers inside strings alone', () => {
        const text = '{"url": "ws://127.0.0.1:8800/agent/x", "glob": "/* not a comment */"}';
        assert.deepEqual(JSON.parse(stripJsonComments(text)), {
            url: 'ws://127.0.0.1:8800/agent/x',
            glob: '/* not a comment */',
        });
    });

    it('honours escaped quotes when tracking string state', () => {
        const text = '{"path": "a\\"//b", "n": 1}';
        assert.deepEqual(JSON.parse(stripJsonComments(text)), { path: 'a"//b', n: 1 });
    });

    it('preserves newlines so parse offsets stay meaningful', () => {
        const stripped = stripJsonComments('{\n// x\n"a":1\n}');
        assert.equal(stripped.split('\n').length, 4);
    });
});

describe('parseConfigText', () => {
    it('rejects documents that are not objects', () => {
        assert.throws(() => parseConfigText('[1,2]', 'x.jsonc'), ConfigError);
    });

    it('reports the file name on invalid JSON', () => {
        assert.throws(() => parseConfigText('{', 'broken.jsonc'), /broken\.jsonc/);
    });
});

describe('mergeConfig', () => {
    it('merges nested objects and replaces arrays', () => {
        const base = { a: { b: 1, c: 2 }, list: [1, 2] };
        const merged = mergeConfig(base, { a: { c: 3 }, list: [9] });
        assert.deepEqual(merged, { a: { b: 1, c: 3 }, list: [9] });
        assert.deepEqual(base, { a: { b: 1, c: 2 }, list: [1, 2] });
    });
});

describe('defaultConfig', () => {
    it('is valid out of the box', () => {
        assert.doesNotThrow(() => validateConfig(defaultConfig()));
    });

    it('returns a fresh object each time', () => {
        const first = defaultConfig();
        first.worker.threads = 4;
        assert.equal(defaultConfig().worker.threads, 1);
    });

    it('defaults to a loopback listener with a 15 second identity hold', () => {
        const config = defaultConfig();
        assert.equal(config.listen.host, '127.0.0.1');
        assert.equal(config.limits.confirmIdentityHoldMs, 15000);
        assert.equal(config.forceKillProcessGroup, false);
    });
});

describe('validateConfig', () => {
    it('requires a panel token off loopback', () => {
        const config = defaultConfig();
        config.listen.host = '0.0.0.0';
        assert.throws(() => validateConfig(config), /panel\.token/);
        config.panel.token = 'secret';
        assert.doesNotThrow(() => validateConfig(config));
    });

    it('rejects an unknown launcher kind', () => {
        const config = defaultConfig();
        config.launcher.kind = 'teleport';
        assert.throws(() => validateConfig(config), /launcher\.kind/);
    });

    it('requires a command template for the command launcher', () => {
        const config = defaultConfig();
        config.launcher.kind = 'command';
        assert.throws(() => validateConfig(config), /launcher\.command/);
        config.launcher.command = ['bash', 'run.sh', '{session}'];
        assert.doesNotThrow(() => validateConfig(config));
    });

    it('keeps the identity hold shorter than the confirmation deadline', () => {
        const config = defaultConfig();
        config.limits.confirmIdentityHoldMs = config.worker.confirmationTimeoutMs;
        assert.throws(() => validateConfig(config), /confirmIdentityHoldMs/);
    });

    it('requires the mock profile to exist when the mock is enabled', () => {
        const config = defaultConfig();
        config.mock.enabled = true;
        config.mock.profile = 'missing';
        assert.throws(() => validateConfig(config), /mock\.profile/);
    });
});

describe('loadConfig', () => {
    it('resolves relative paths against the configuration file', () => {
        const dir = tempDir();
        const file = join(dir, 'hub.config.jsonc');
        writeFileSync(file, `{
            // paths are file-relative
            "dataDir": "./state",
            "worker": { "bin": "../bin/simplex_worker" }
        }`);
        const { config } = loadConfig({ file });
        assert.equal(config.dataDir, join(dir, 'state'));
        assert.equal(config.worker.bin, join(dir, '..', 'bin', 'simplex_worker'));
        assert.equal(config.listen.port, 8800);
    });

    it('applies command-line overrides on top of the file', () => {
        const dir = tempDir();
        const file = join(dir, 'hub.config.jsonc');
        writeFileSync(file, '{ "listen": { "port": 9000 }, "worker": { "threads": 2 } }');
        const { config } = loadConfig({
            file,
            overrides: { listen: { port: 9100 }, dataDir: './runtime' },
            cwd: dir,
        });
        assert.equal(config.listen.port, 9100);
        assert.equal(config.listen.host, '127.0.0.1');
        assert.equal(config.worker.threads, 2);
        assert.equal(config.dataDir, join(dir, 'runtime'));
    });

    it('fails with a clear message for a missing explicit file', () => {
        assert.throws(() => loadConfig({ file: '/nonexistent/hub.config.jsonc' }),
            /configuration file not found/);
    });

    it('loads the shipped example configuration', () => {
        const file = join(import.meta.dirname, '..', 'hub.config.example.jsonc');
        const { config } = loadConfig({ file });
        assert.equal(config.launcher.kind, 'simplex-worker');
        assert.ok(config.providerProfiles.deepseek);
        assert.ok(config.providerProfiles.mock);
    });
});
