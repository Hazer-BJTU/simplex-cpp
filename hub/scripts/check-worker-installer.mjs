#!/usr/bin/env node
/** Run the emitted installer from an unpacked npm package with production dependencies only. */
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync, symlinkSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const installed = resolve(process.argv[2]);
const root = mkdtempSync(join(tmpdir(), 'simplex-packed-installer-'));
try {
    const home = join(root, 'home');
    mkdirSync(home);
    const cli = join(installed, 'dist/bin/simplex-hub.js');
    const result = execFileSync(process.execPath, [cli, 'install-worker', '--help'], {
        cwd: home, env: { ...process.env, HOME: home }, encoding: 'utf8', timeout: 30_000,
    });
    assert.match(result, /--allow-downgrade/);
    assert.equal(existsSync(join(home, '.simplex')), false);

    const version = '0.2.0';
    const name = `simplex-worker-v${version}-linux-x86_64-glibc2.34`;
    const tree = join(root, name);
    for (const dir of ['bin/prompts/operations', 'bin/schemas/process', 'bin/plugins/llm', 'lib', 'third_party_licenses']) {
        mkdirSync(join(tree, dir), { recursive: true });
    }
    for (const file of ['bin/config.example.yaml', 'bin/prompts/coding_agent.yaml',
        'bin/prompts/operations/compact.yaml', 'bin/schemas/process/skill.yaml',
        ...['run_command', 'spawn_process', 'read_process', 'send_process', 'poll_process'].map(name => `bin/schemas/process/${name}.yaml`),
        'bin/plugins/llm/libllm_fixture.so', 'lib/libfixture.so.1', 'LICENSE', 'README.md']) {
        writeFileSync(join(tree, file), 'fixture');
    }
    writeFileSync(join(tree, 'bin/simplex'), '#!/bin/bash\nprintf "packed worker help\\n"\n', { mode: 0o755 });
    writeFileSync(join(tree, 'bin/simplex_worker'), '#!/bin/bash\nexit 0\n', { mode: 0o755 });
    symlinkSync('libfixture.so.1', join(tree, 'lib/libfixture.so'));
    const archive = join(root, 'fixture.tar.gz');
    execFileSync('tar', ['-czf', archive, '-C', root, name]);
    const bytes = readFileSync(archive);
    const release = { source: 'github', version, root: name, archive: `${name}.tar.gz`, archiveUrl: 'https://fixture/archive', checksumUrl: 'https://fixture/checksums' };
    const checksum = `${createHash('sha256').update(bytes).digest('hex')}  ${release.archive}\n`;
    const { installWorker } = await import(pathToFileURL(join(installed, 'dist/src/install/command.js')));
    const destination = join(home, 'worker');
    const dependencies = {
        home, release: async () => release, checkHost: async () => {},
        checkStartup: async directory => {
            assert.match(execFileSync(join(directory, 'bin/simplex'), ['run', '--help'], { encoding: 'utf8' }), /packed worker help/);
        },
        fetcher: async url => new Response(url === release.checksumUrl ? checksum : bytes),
        interactive: false,
    };
    assert.equal(await installWorker(['--directory', destination, '--no-update-path'], dependencies), 0);
    assert.equal(existsSync(join(destination, 'bin/simplex')), true);
    assert.equal(readFileSync(join(destination, 'lib/libfixture.so'), 'utf8'), 'fixture');
    assert.equal(await installWorker(['--directory', destination, '--update-path'], dependencies), 0);
    assert.match(readFileSync(join(home, '.bashrc'), 'utf8'), /simplex-hub managed worker PATH/);
    process.stdout.write('Validated packed installer help, fixture installation, current-version PATH update, and production dependencies\n');
} finally {
    rmSync(root, { recursive: true, force: true });
}
