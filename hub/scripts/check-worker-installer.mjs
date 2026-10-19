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

    assert.match(result, /--platform/);
    assert.match(result, /linux-amd64-standard/);
    assert.match(result, /linux-amd64-bundled/);
    const { WORKER_PLATFORMS } = await import(pathToFileURL(join(installed, 'dist/src/install/platform.js')));
    const { smokeCheck } = await import(pathToFileURL(join(installed, 'dist/src/install/host.js')));
    for (const platform of Object.keys(WORKER_PLATFORMS)) {
        const version = '0.2.0';
        const name = `simplex-worker-v${version}-${WORKER_PLATFORMS[platform].suffix}`;
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
        // Managed sessions execute the adjacent manager; validateTree() requires
        // it and the bundled smoke check runs it with --help.
        writeFileSync(join(tree, 'bin/simplex_manager'), '#!/bin/bash\nexit 0\n', { mode: 0o755 });
        if (platform === 'linux-amd64-bundled') {
            for (const folder of ['share/certs', 'share/openssl']) mkdirSync(join(tree, folder), { recursive: true });
            for (const file of ['share/certs/ca-certificates.crt', 'share/openssl/openssl.cnf',
                'lib/libssl.so.3', 'lib/libcrypto.so.3']) writeFileSync(join(tree, file), 'fixture');
            writeFileSync(join(tree, 'bin/simplex_worker.bin'), '#!/bin/bash\nprintf "packed worker help\\n"\n', { mode: 0o755 });
            writeFileSync(join(tree, 'bin/simplex_worker'),
                readFileSync(new URL('../../docker/musl-worker-launcher.sh', import.meta.url)), { mode: 0o755 });
            writeFileSync(join(tree, 'bin/simplex'), '#!/bin/bash\nexec "$(dirname "$0")/simplex_worker" "$@"\n', { mode: 0o755 });
            writeFileSync(join(tree, 'lib/ld-musl-x86_64.so.1'), `#!/bin/bash
    if [[ "$3" == --list ]]; then
      printf '/lib/ld-musl-x86_64.so.1 (0x10)\nlibc.musl-x86_64.so.1 => /lib/ld-musl-x86_64.so.1 (0x10)\nlibssl.so.3 => %s/libssl.so.3 (0x1)\nlibcrypto.so.3 => %s/libcrypto.so.3 (0x2)\n' "$2" "$2"
    else
      exec "$3" "\${@:4}"
    fi
    `, { mode: 0o755 });
        }
        symlinkSync('libfixture.so.1', join(tree, 'lib/libfixture.so'));
        const archive = join(root, 'fixture.tar.gz');
        execFileSync('tar', ['-czf', archive, '-C', root, name]);
        const bytes = readFileSync(archive);
        const release = { source: 'github', platform, version, root: name, archive: `${name}.tar.gz`, archiveUrl: 'https://fixture/archive', checksumUrl: 'https://fixture/checksums' };
        const checksum = `${createHash('sha256').update(bytes).digest('hex')}  ${release.archive}\n`;
        const { installWorker } = await import(pathToFileURL(join(installed, 'dist/src/install/command.js')));
        const destination = join(home, platform);
        const dependencies = {
            home, release: async () => release, checkHost: async () => {},
            checkStartup: async (directory, selected) => {
                assert.equal(selected, platform);
                if (platform === 'linux-amd64-bundled') return smokeCheck(directory, platform);
                assert.match(execFileSync(join(directory, 'bin/simplex'), ['run', '--help'], { encoding: 'utf8' }), /packed worker help/);
            },
            fetcher: async url => new Response(url === release.checksumUrl ? checksum : bytes),
            interactive: false,
        };
        assert.equal(await installWorker(['--platform', platform, '--directory', destination, '--no-update-path'], dependencies), 0);
        assert.equal(existsSync(join(destination, 'bin/simplex')), true);
        assert.equal(readFileSync(join(destination, 'lib/libfixture.so'), 'utf8'), 'fixture');
        assert.equal(await installWorker(['--platform', platform, '--directory', destination, '--update-path'], dependencies), 0);
        assert.match(readFileSync(join(home, '.bashrc'), 'utf8'), /simplex-hub managed worker PATH/);
    }
    process.stdout.write('Validated both packed installer platform paths, help, fixture installation, current-version PATH updates, and production dependencies\n');
} finally {
    rmSync(root, { recursive: true, force: true });
}
