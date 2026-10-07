import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { chmodSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import { workerUnavailableReason } from './helpers/worker-preconditions.js';

const helper = fileURLToPath(new URL('./helpers/e2e.js', import.meta.url));

function fixture(t) {
    const bin = mkdtempSync(join(tmpdir(), 'simplex-e2e-preconditions-'));
    t.after(() => rmSync(bin, { recursive: true, force: true }));
    const worker = join(bin, 'simplex_worker');
    const write = (path, content = 'fixture') => {
        const file = join(bin, path);
        mkdirSync(dirname(file), { recursive: true });
        writeFileSync(file, content);
    };
    write('simplex_worker', '#!/bin/sh\nexit 0\n');
    chmodSync(worker, 0o755);
    write('prompts/coding_agent.yaml');
    const runtime = [
        'prompts/operations/compact.yaml', 'prompts/operations/auto_compact.yaml',
        'prompts/operations/auto_compact_continue.yaml', 'plugins/llm/libllm_deepseek.so',
        ...['poll_process', 'read_process', 'run_command', 'send_process', 'spawn_process', 'skill']
            .map((name) => `schemas/process/${name}.yaml`),
    ];
    for (const path of runtime) {
        write(path);
    }
    return { bin, worker, write, runtime };
}

test('missing and non-executable workers are unavailable even in local mode', (t) => {
    const f = fixture(t);
    assert.equal(workerUnavailableReason(f.worker), null);
    chmodSync(f.worker, 0o644);
    assert.match(workerUnavailableReason(f.worker), /prerequisite/);
    assert.match(workerUnavailableReason(join(f.bin, 'missing')), /prerequisite/);
});

test('strict CI validates every required runtime input; local mode can still skip', (t) => {
    const f = fixture(t);
    assert.equal(workerUnavailableReason(f.worker, { required: true }), null);
    for (const path of ['prompts/coding_agent.yaml', ...f.runtime]) {
        rmSync(join(f.bin, path));
        assert.match(workerUnavailableReason(f.worker, { required: true }), /prerequisite/);
        f.write(path);
    }
    rmSync(join(f.bin, 'prompts/coding_agent.yaml'));
    mkdirSync(join(f.bin, 'prompts/coding_agent.yaml'));
    assert.match(workerUnavailableReason(f.worker), /not a regular file/);
});

test('strict CI fails when the executable cannot load/start', (t) => {
    const f = fixture(t);
    f.write('simplex_worker', '#!/bin/sh\necho "missing shared library" >&2\nexit 127\n');
    assert.match(workerUnavailableReason(f.worker, { required: true }), /missing shared library/);
    assert.equal(workerUnavailableReason(f.worker), null);
    f.write('simplex_worker', '#!/nonexistent/loader\n');
    assert.match(workerUnavailableReason(f.worker, { required: true }), /worker cannot start/);
});

test('helper import skips locally but throws in strict CI instead of marking the suite skipped', (t) => {
    const f = fixture(t);
    const check = (required) => spawnSync(process.execPath, ['--input-type=module', '-e',
        `const helper = await import(${JSON.stringify(helper)}); console.log(helper.e2eSkip);`], {
        encoding: 'utf8',
        env: { ...process.env, SIMPLEX_WORKER_BIN: f.worker,
            SIMPLEX_E2E_REQUIRED: required ? '1' : '0' },
    });
    rmSync(join(f.bin, 'prompts/coding_agent.yaml'));
    const local = check(false);
    assert.equal(local.status, 0, local.stderr);
    assert.match(local.stdout, /real worker unavailable/);
    const required = check(true);
    assert.notEqual(required.status, 0);
    assert.match(required.stderr, /Required real-worker E2E unavailable/);
    f.write('prompts/coding_agent.yaml');
    const available = check(true);
    assert.equal(available.status, 0, available.stderr);
    assert.equal(available.stdout.trim(), 'false');
});
