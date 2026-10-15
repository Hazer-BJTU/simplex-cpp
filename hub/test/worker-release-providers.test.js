/** Exercise the actual glibc archive checks, including provider-set changes. */
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';

const repo = fileURLToPath(new URL('../../', import.meta.url));
test('glibc packaging and extracted-archive checks reject missing/empty providers', t => {
    const root = mkdtempSync(join(tmpdir(), 'simplex-release-providers-'));
    t.after(() => rmSync(root, { recursive: true, force: true }));
    const stage = join(root, 'stage');
    for (const dir of ['bin/plugins/llm', 'bin/prompts', 'bin/schemas/process', 'lib']) {
        mkdirSync(join(stage, dir), { recursive: true });
    }
    // A real host ELF allows ldd and the CLI smoke check without a worker build.
    for (const name of ['simplex', 'simplex_worker']) copyFileSync('/bin/true', join(stage, 'bin', name));
    writeFileSync(join(stage, 'bin/prompts/coding_agent.yaml'), 'fixture');
    writeFileSync(join(stage, 'bin/schemas/process/skill.yaml'), 'fixture');
    const output = join(root, 'output');
    const pack = () => execFileSync('bash', [join(repo, 'docker/package-worker-release.sh'), stage, output], { encoding: 'utf8', stdio: 'pipe' }).trim();
    assert.throws(pack, /no model-provider plugin/);
    const provider = join(stage, 'bin/plugins/llm/libllm_alternative.so');
    writeFileSync(provider, 'fixture');
    const archive = pack();
    const check = () => execFileSync('bash', [join(repo, 'docker/check-worker-release.sh'), archive], { stdio: 'pipe' });
    assert.doesNotThrow(check);
    const extracted = join(root, 'extracted');
    mkdirSync(extracted);
    execFileSync('tar', ['-xzf', archive, '-C', extracted]);
    const name = archive.split('/').at(-1).replace(/\.tar\.gz$/, '');
    writeFileSync(join(extracted, name, 'bin/plugins/llm/libllm_alternative.so'), '');
    execFileSync('tar', ['-czf', archive, '-C', extracted, name]);
    assert.throws(check, /invalid model-provider plugin/);
    writeFileSync(provider, '');
    assert.throws(pack, /invalid model-provider plugin/);
});

test('musl packager and relocated runtime reject an empty provider set before other probes', t => {
    const root = mkdtempSync(join(tmpdir(), 'simplex-musl-providers-'));
    t.after(() => rmSync(root, { recursive: true, force: true }));
    for (const directory of ['bin/plugins/llm', 'lib']) {
        mkdirSync(join(root, directory), { recursive: true });
    }
    for (const file of ['bin/simplex', 'bin/simplex_worker', 'bin/simplex_worker.bin',
        'lib/ld-musl-x86_64.so.1']) copyFileSync('/bin/true', join(root, file));
    assert.throws(() => execFileSync('bash', [join(repo, 'docker/package-musl-worker.sh'),
        root, join(root, 'output')], { stdio: 'pipe' }), /no model-provider plugin/);
    assert.throws(() => execFileSync('python3', [join(repo, 'docker/test-musl-runtime.py'),
        root, repo], { stdio: 'pipe' }), /no model-provider plugin/);
});
