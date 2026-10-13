/** Offline installer regressions exercise real archives, filesystem transactions, and Bash. */
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdtemp, mkdir, writeFile, readFile, rm, readdir, chmod, symlink, lstat, rename } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { PassThrough } from 'node:stream';
import { test } from 'node:test';
import * as tar from 'tar';
import { WORKER_PLATFORMS } from '../src/install/platform.ts';
import { resolveRelease } from '../src/install/source.ts';
import { validateArchive, prepareArchive } from '../src/install/archive.ts';
import { installationAction, compareVersions, normalizeVersion, METADATA, readInstalled, validateTree } from '../src/install/version.ts';
import { InstallationTransaction, destinationPath } from '../src/install/transaction.ts';
import { acquireLock, exists } from '../src/install/files.ts';
import { START, END, bashQuote, editBashrc, updateBashrc } from '../src/install/bashrc.ts';
import { installWorker, parseInstallArguments, confirmPath } from '../src/install/command.ts';
import { smokeCheck, validatePlatform, validateHost } from '../src/install/host.ts';

const exec = promisify(execFile);
async function sandbox(t) {
    const root = await mkdtemp(join(tmpdir(), 'simplex-installer-'));
    t.after(() => rm(root, { recursive: true, force: true }));
    const home = join(root, 'home');
    await mkdir(home);
    return { root, home, destination: join(home, 'worker') };
}

function release(version = '0.2.0', platform = 'linux-amd64-standard') {
    const root = `simplex-worker-v${version}-${WORKER_PLATFORMS[platform].suffix}`;
    return { source: 'github', platform, version, root, archive: `${root}.tar.gz`,
        archiveUrl: 'https://fixture/worker', checksumUrl: 'https://fixture/SHA256SUMS' };
}

async function fixture(t, { version = '0.2.0', broken = false, platform = 'linux-amd64-standard' } = {}) {
    const context = await sandbox(t);
    const selected = release(version, platform);
    const tree = join(context.root, selected.root);
    for (const dir of ['bin/prompts/operations', 'bin/schemas/process', 'bin/plugins/llm', 'lib', 'third_party_licenses']) {
        await mkdir(join(tree, dir), { recursive: true });
    }
    for (const file of ['bin/config.example.yaml', 'bin/prompts/coding_agent.yaml',
        'bin/prompts/operations/compact.yaml', 'bin/schemas/process/skill.yaml',
        ...['run_command', 'spawn_process', 'read_process', 'send_process', 'poll_process'].map(name => `bin/schemas/process/${name}.yaml`),
        'bin/plugins/llm/libllm_example.so', 'LICENSE', 'README.md', 'lib/libfixture.so.1']) {
        await writeFile(join(tree, file), 'fixture');
    }
    await writeFile(join(tree, 'bin/simplex'), '#!/bin/bash\nexec "$(dirname "$0")/simplex_worker" "$@"\n', { mode: 0o755 });
    await writeFile(join(tree, 'bin/simplex_worker'), '#!/bin/bash\nprintf "worker help\\n"\n', { mode: broken ? 0o644 : 0o755 });
    if (platform === 'linux-amd64-bundled') {
        await mkdir(join(tree, 'share/certs'), { recursive: true });
        await mkdir(join(tree, 'share/openssl'), { recursive: true });
        await writeFile(join(tree, 'share/certs/ca-certificates.crt'), 'fixture CA');
        await writeFile(join(tree, 'share/openssl/openssl.cnf'), 'fixture OpenSSL config');
        await writeFile(join(tree, 'lib/libssl.so.3'), 'fixture SSL');
        await writeFile(join(tree, 'lib/libcrypto.so.3'), 'fixture crypto');
        await rename(join(tree, 'bin/simplex_worker'), join(tree, 'bin/simplex_worker.bin'));
        await writeFile(join(tree, 'bin/simplex_worker'),
            await readFile(new URL('../../docker/musl-worker-launcher.sh', import.meta.url)), { mode: 0o755 });
        // Emulate the loader CLI rather than treating the wrapper as an ELF.
        await writeFile(join(tree, 'lib/ld-musl-x86_64.so.1'), `#!/bin/bash
[[ -z "\${LD_LIBRARY_PATH+x}" && -z "\${LD_PRELOAD+x}" && -z "\${LD_AUDIT+x}" ]] || exit 1
[[ "$1" == --library-path ]] || exit 2
if [[ "$3" == --list ]]; then
  [[ "$4" == */bin/simplex_worker.bin ]] || exit 3
  printf '/lib/ld-musl-x86_64.so.1 (0x10)\nlibc.musl-x86_64.so.1 => /lib/ld-musl-x86_64.so.1 (0x10)\nlibssl.so.3 => %s/libssl.so.3 (0x1)\nlibcrypto.so.3 => %s/libcrypto.so.3 (0x2)\n' "$2" "$2"
else
  exec "$3" "\${@:4}"
fi
`, { mode: 0o755 });
    }
    await symlink('libfixture.so.1', join(tree, 'lib/libfixture.so'));
    const archive = join(context.root, 'fixture.tar.gz');
    await tar.c({ file: archive, gzip: true, cwd: context.root }, [selected.root]);
    const bytes = await readFile(archive);
    const checksum = `${createHash('sha256').update(bytes).digest('hex')}  ${selected.archive}\n`;
    const fetcher = async url => new Response(url === selected.checksumUrl ? checksum : bytes);
    return { ...context, tree, archive, bytes, checksum, selected, fetcher };
}

async function install(context, argv = [], dependencies = {}) {
    let output = '';
    const stream = new PassThrough();
    stream.on('data', chunk => { output += chunk; });
    const code = await installWorker(['--platform', context.selected.platform, '--directory', context.destination, ...argv], {
        home: context.home, checkHost: async () => {}, checkStartup: async (directory, platform) => {
            assert.equal(platform, context.selected.platform);
            if (platform === 'linux-amd64-bundled') return smokeCheck(directory, platform);
            const { stdout } = await exec(join(directory, 'bin/simplex'), ['run', '--help']);
            assert.match(stdout, /worker help/);
        }, release: async () => context.selected, fetcher: context.fetcher,
        output: stream, interactive: false, ...dependencies,
    });
    return { code, output };
}

/** Craft exact header fields for cases an ordinary archive creator would sanitize. */
function rawTar(root, additions) {
    const entries = [{ path: `${root}/`, type: 'Directory', mode: 0o755, size: 0 }, ...additions];
    const blocks = entries.flatMap(entry => {
        const header = new tar.Header({ mode: 0o644, size: 0, ...entry });
        header.encode();
        const content = Buffer.from(entry.contents ?? '');
        return [header.block, content, Buffer.alloc((512 - content.length % 512) % 512)];
    });
    return Buffer.concat([...blocks, Buffer.alloc(1024)]);
}

test('release resolver selects stable official assets and explicit tags', async () => {
    const asset = name => ({ name, browser_download_url: `https://github.com/Hazer-BJTU/simplex-cpp/releases/download/v0.2.0/${name}` });
    const document = { draft: false, prerelease: false, tag_name: 'v0.2.0',
        assets: [asset(release().archive.replace('-glibc2.34', '-musl')), asset(release().archive), asset('SHA256SUMS')] };
    let url;
    const fetcher = async value => { url = value; return new Response(JSON.stringify(document)); };
    assert.equal((await resolveRelease('github', 'linux-amd64-standard', undefined, fetcher)).version, '0.2.0');
    assert.match(url, /\/releases\/latest$/);
    await resolveRelease('github', 'linux-amd64-standard', '0.2.0', fetcher);
    assert.match(url, /\/tags\/v0\.2\.0$/);
    await assert.rejects(resolveRelease('mirror', 'linux-amd64-standard', undefined, fetcher), /Unsupported source/);
    await assert.rejects(resolveRelease('github', 'linux-amd64-standard', 'v0.2.1', fetcher), /does not match/);
    document.prerelease = true;
    await assert.rejects(resolveRelease('github', 'linux-amd64-standard', undefined, fetcher), /stable/);
    document.prerelease = false;
    document.assets.pop();
    await assert.rejects(resolveRelease('github', 'linux-amd64-standard', undefined, fetcher), /SHA256SUMS/);
    document.assets.push(asset('SHA256SUMS'), asset('SHA256SUMS'));
    await assert.rejects(resolveRelease('github', 'linux-amd64-standard', undefined, fetcher), /one SHA256SUMS/);
    await assert.rejects(resolveRelease('github', 'linux-amd64-standard', undefined, async () => new Response('', { status: 404 })), /HTTP 404/);
    document.assets = [asset(release().archive), { name: 'SHA256SUMS', browser_download_url: 'https://untrusted.example/sums' }];
    await assert.rejects(resolveRelease('github', 'linux-amd64-standard', undefined, fetcher), /Unexpected official/);
});

test('platform and argument decisions are explicit and numerically ordered', () => {
    assert.doesNotThrow(() => validatePlatform('linux-amd64-standard', 'linux', 'x64', '2.34'));
    for (const args of [['darwin', 'x64', '2.40'], ['linux', 'arm64', '2.40'], ['linux', 'x64', '2.33'], ['linux', 'x64', undefined]]) {
        assert.throws(() => validatePlatform('linux-amd64-standard', ...args), /require/);
    }
    assert.equal(compareVersions('0.10.0', '0.9.9'), 1);
    assert.equal(compareVersions('1.0.0', '1.0.0'), 0);
    assert.equal(compareVersions('0.2.0', '0.10.0'), -1);
    assert.throws(() => normalizeVersion('v0.2.0-beta'), /Invalid/);
    const options = parseInstallArguments(['--platform', 'linux-amd64-standard', '--overwrite']);
    assert.equal(options.allowDowngrade, false);
    assert.equal(options.updatePath, undefined);
    assert.throws(() => parseInstallArguments(['--update-path', '--no-update-path']), /cannot be combined/);
    assert.throws(() => parseInstallArguments(['--source', 'other']), /Unsupported/);
    assert.throws(() => parseInstallArguments(['--directory']), /requires/);
    assert.throws(() => parseInstallArguments(['--config', 'ignored']), /Unknown/);
    assert.throws(() => installationAction({ version: '0.3.0' }, '0.2.0', { platform: 'linux-amd64-standard', reinstall: true, allowDowngrade: false }), /allow-downgrade/);
});

test('archive preparation validates and retains internal library links', async t => {
    const f = await fixture(t);
    const work = join(f.root, 'work');
    await mkdir(work);
    const tree = await prepareArchive(f.selected, work, f.fetcher);
    assert.equal(await readFile(join(tree, 'lib/libfixture.so'), 'utf8'), 'fixture');
    assert.equal((await lstat(join(tree, 'bin/simplex'))).mode & 0o111, 0o111);
});

test('unsafe, malformed, truncated and conflicting tar entries are rejected', async t => {
    const { root } = await sandbox(t);
    const name = release().root;
    for (const entries of [
        [{ path: `${name}/../escape`, type: 'File' }],
        [{ path: '/absolute', type: 'File' }],
        [{ path: `${name}/link`, type: 'SymbolicLink', linkpath: '../../escape' }],
        [{ path: `${name}/link`, type: 'SymbolicLink', linkpath: '/etc/passwd' }],
        [{ path: `${name}/link`, type: 'SymbolicLink', linkpath: 'link' }],
        [{ path: `${name}/lib`, type: 'SymbolicLink', linkpath: 'file' }, { path: `${name}/file`, type: 'File' }, { path: `${name}/lib/evil`, type: 'File' }],
        [{ path: `${name}/same`, type: 'File' }, { path: `${name}/same`, type: 'File' }],
        [{ path: `${name}/fifo`, type: 'FIFO' }],
        [{ path: `${name}/file`, type: 'File', mode: 0o4755 }],
        [{ path: `${name}/missing`, type: 'Link', linkpath: `${name}/absent` }],
    ]) {
        const path = join(root, 'unsafe.tar');
        await writeFile(path, rawTar(name, entries));
        await assert.rejects(validateArchive(path, name));
    }
    const path = join(root, 'bad.tar');
    await writeFile(path, rawTar(name, []).subarray(0, 700));
    await assert.rejects(validateArchive(path, name), /Truncated/);
    await writeFile(path, Buffer.alloc(1536, 65));
    await assert.rejects(validateArchive(path, name));
});

test('preparation and startup failures preserve the current tree and clean staging', async t => {
    const f = await fixture(t);
    await install(f);
    const sentinel = join(f.destination, 'sentinel');
    await writeFile(sentinel, 'keep');
    for (const fetcher of [
        async url => new Response(url === f.selected.checksumUrl ? f.checksum : Buffer.from('bad gzip')),
        async url => new Response(url === f.selected.checksumUrl ? `invalid\n` : f.bytes),
        async url => new Response('', { status: 503 }),
    ]) {
        await assert.rejects(install(f, ['--reinstall'], { fetcher }));
        assert.equal(await readFile(sentinel, 'utf8'), 'keep');
        assert.deepEqual(await readdir(new InstallationTransaction(f.destination).work), ['lock']);
    }
    const truncated = f.bytes.subarray(0, f.bytes.length - 30);
    const checksum = `${createHash('sha256').update(truncated).digest('hex')}  ${f.selected.archive}\n`;
    await assert.rejects(install(f, ['--reinstall'], { fetcher: async url => new Response(url === f.selected.checksumUrl ? checksum : truncated) }));
    await assert.rejects(install(f, ['--reinstall'], { checkStartup: async () => { throw new Error('startup failed'); } }), /startup failed/);
    assert.equal(await readFile(sentinel, 'utf8'), 'keep');
});

test('fresh/current/reinstall/upgrade/downgrade and incomplete current installations', async t => {
    const f = await fixture(t);
    assert.equal((await install(f)).code, 0);
    const sentinel = join(f.destination, 'stale');
    await writeFile(sentinel, 'old');
    const current = await install(f, [], { fetcher: async () => { throw new Error('no-op must not download'); } });
    assert.match(current.output, /already current/);
    assert.equal(await exists(sentinel), true);
    await install(f, ['--reinstall']);
    assert.equal(await exists(sentinel), false);
    await chmod(join(f.destination, 'bin/simplex_worker'), 0o644);
    await assert.rejects(install(f), /--reinstall/);
    await install(f, ['--reinstall']);
    const newer = await fixture(t, { version: '0.10.0' });
    await install({ ...newer, home: f.home, destination: f.destination });
    await assert.rejects(install(f, ['--overwrite']), /allow-downgrade/);
    await install(f, ['--allow-downgrade']);
    assert.equal(JSON.parse(await readFile(join(f.destination, METADATA))).version, '0.2.0');
});

test('unknown directories require overwrite; dangerous and symlinked targets remain forbidden', async t => {
    const f = await fixture(t);
    await mkdir(f.destination);
    await writeFile(join(f.destination, 'unrelated'), 'data');
    await assert.rejects(install(f), /--overwrite/);
    await install(f, ['--overwrite']);
    assert.equal(await exists(join(f.destination, 'unrelated')), false);
    for (const target of ['/', '/tmp', '/bin', '/usr/bin', '/usr/local/bin', f.home, join(f.home, '..'), process.cwd()]) {
        await assert.rejects(destinationPath(target, f.home), /dangerous|symlink/);
    }
    const link = join(f.home, 'linked');
    await symlink(f.destination, link);
    await assert.rejects(destinationPath(link, f.home), /symlink/);
    assert.equal(await destinationPath(join(f.home, 'new', 'deep', 'worker'), f.home), join(f.home, 'new/deep/worker'));
});

test('replacement failure rolls back before returning', async t => {
    const { home, destination } = await sandbox(t);
    await mkdir(destination);
    await writeFile(join(destination, 'old'), 'old');
    const transaction = new InstallationTransaction(destination);
    await transaction.open();
    const tree = join(transaction.work, 'new');
    await mkdir(tree);
    await writeFile(join(tree, 'new'), 'new');
    await assert.rejects(transaction.replace(tree, async () => { throw new Error('injected publish failure'); }), /injected/);
    assert.equal(await readFile(join(destination, 'old'), 'utf8'), 'old');
    assert.equal(await exists(join(destination, 'new')), false);
    await transaction.close();
});

test('interrupted transactions recover before another installation', async t => {
    const { destination } = await sandbox(t);
    const tx = new InstallationTransaction(destination);
    await mkdir(tx.work, { mode: 0o700 });
    await mkdir(join(tx.work, 'previous'));
    await writeFile(join(tx.work, 'previous/old'), 'old');
    await mkdir(destination);
    await writeFile(join(destination, 'partial'), 'new');
    await writeFile(join(tx.work, 'transaction.json'), JSON.stringify({ hadDestination: true }));
    await tx.open();
    assert.equal(await readFile(join(destination, 'old'), 'utf8'), 'old');
    assert.equal(await exists(join(destination, 'partial')), false);
    await tx.close();
    // A committed transaction's leftover backup never replaces the new tree.
    await mkdir(join(tx.work, 'previous'));
    await writeFile(join(tx.work, 'previous/stale'), 'stale');
    await tx.open();
    assert.equal(await exists(join(destination, 'old')), true);
    assert.equal(await exists(join(destination, 'stale')), false);
    await tx.close();
});

test('kernel locks exclude concurrent installers and release after owner process death', async t => {
    const { destination } = await sandbox(t);
    const first = new InstallationTransaction(destination);
    await first.open();
    await assert.rejects(new InstallationTransaction(destination).open(), /lock/);
    await first.close();
    const module = new URL('../src/install/files.ts', import.meta.url).href;
    const lock = join(first.work, 'lock');
    const child = spawn(process.execPath, ['--input-type=module', '-e',
        `import {acquireLock} from ${JSON.stringify(module)}; await acquireLock(process.argv[1]); console.log('ready');`, lock], { stdio: ['ignore', 'pipe', 'pipe'] });
    await new Promise((done, reject) => { child.stdout.once('data', done); child.once('error', reject); });
    const closed = new Promise(done => child.once('close', done));
    child.kill('SIGKILL');
    await closed;
    // The helper exits on pipe EOF; wait for its OS lock to be released.
    for (let attempt = 0; ; attempt++) {
        try { const release = await acquireLock(lock); await release(); break; }
        catch (error) { if (attempt > 50) throw error; await new Promise(done => setTimeout(done, 10)); }
    }
});

test('PATH editor is idempotent, replaces duplicates, and refuses malformed blocks', () => {
    const contents = '# unrelated\nexport X=1\n';
    const once = editBashrc(contents, '/worker one');
    assert.equal(editBashrc(once, '/worker one'), once);
    const twice = editBashrc(`${once}${once}`, '/other');
    assert.equal(twice.split(START).length - 1, 1);
    assert.equal(twice.split('export X=1').length - 1, 2);
    assert.match(twice, /\/other\/bin/);
    assert.doesNotMatch(twice, /worker one/);
    for (const broken of [`${START}\n`, `${END}\n`, `${START}\n${START}\n${END}\n`, `# ${START}\n${END}\n`]) {
        assert.throws(() => editBashrc(broken, '/worker'), /Malformed|Unterminated/);
    }
});

test('Bash quoting preserves hostile literal paths without executing them', async t => {
    const { root } = await sandbox(t);
    const marker = join(root, 'executed');
    const path = `${root}/space ' quote $HOME $(touch ${marker}) \`touch ${marker}\``;
    const script = `${editBashrc('', path)}printf '%s' "$PATH"`;
    const { stdout } = await exec('bash', ['--noprofile', '--norc', '-c', script], { env: { PATH: '/usr/bin:/bin', HOME: root } });
    assert.equal(stdout, `${path}/bin:/usr/bin:/bin`);
    assert.equal(await exists(marker), false);
    assert.equal((await exec('bash', ['--noprofile', '--norc', '-c', `printf '%s' ${bashQuote(path)}`])).stdout, path);
});

test('PATH update preserves mode/content, rejects symlinks, and has its own lock', async t => {
    const { home, destination } = await sandbox(t);
    const bashrc = join(home, '.bashrc');
    await writeFile(bashrc, '# keep\n', { mode: 0o640 });
    await updateBashrc(home, destination);
    assert.equal((await lstat(bashrc)).mode & 0o777, 0o640);
    assert.match(await readFile(bashrc, 'utf8'), /^# keep\n/);
    const unlock = await acquireLock(join(home, '.simplex-hub-bashrc.lock'));
    await assert.rejects(updateBashrc(home, '/other'), /lock/);
    await unlock();
    await updateBashrc(home, '/other');
    assert.equal((await readFile(bashrc, 'utf8')).split(START).length - 1, 1);
    await rename(bashrc, `${bashrc}.real`);
    await symlink(`${bashrc}.real`, bashrc);
    await assert.rejects(updateBashrc(home, destination), /symlink/);
    assert.equal((await lstat(bashrc)).isSymbolicLink(), true);
});

test('current no-op still updates PATH; PATH failures do not undo installation', async t => {
    const f = await fixture(t);
    const first = await install(f);
    assert.match(first.output, /was not changed/);
    assert.equal(await exists(join(f.home, '.bashrc')), false);
    await install(f, ['--update-path']);
    assert.match(await readFile(join(f.home, '.bashrc'), 'utf8'), /managed worker PATH/);
    await writeFile(join(f.home, '.bashrc'), `${START}\n`);
    const outcome = await install(f, ['--update-path', '--reinstall']);
    assert.equal(outcome.code, 1);
    assert.match(outcome.output, /Worker is installed, but PATH update failed/);
    assert.equal(await exists(join(f.destination, 'bin/simplex')), true);
    assert.equal(await readFile(join(f.home, '.bashrc'), 'utf8'), `${START}\n`);
});

test('confirmation handles accepted input, EOF and cancellation', async () => {
    for (const method of ['yes', 'eof', 'abort']) {
        const input = new PassThrough();
        const controller = new AbortController();
        const response = confirmPath(input, new PassThrough(), controller.signal);
        if (method === 'yes') input.write('yes\n');
        else if (method === 'eof') input.end();
        else controller.abort();
        assert.equal(await response, method === 'yes');
        input.destroy();
    }
});

test('CLI install help works with broken Hub config and no startup side effects', async t => {
    const { root, home } = await sandbox(t);
    await writeFile(join(root, 'hub.config.jsonc'), 'broken');
    const cli = resolve(import.meta.dirname, '../bin/simplex-hub.ts');
    const { stdout } = await exec(process.execPath, [cli, 'install-worker', '--help'], { cwd: root, env: { ...process.env, HOME: home } });
    assert.match(stdout, /--allow-downgrade/);
    assert.deepEqual(await readdir(home), []);
    assert.deepEqual((await readdir(root)).sort(), ['home', 'hub.config.jsonc']);
    assert.match((await exec(process.execPath, [cli, '--help'])).stdout, /install-worker/);
    const manifest = JSON.parse(await readFile(resolve(import.meta.dirname, '../package.json'), 'utf8'));
    assert.equal((await exec(process.execPath, [cli, '--version'])).stdout.trim(), manifest.version);
});

test('actual process interruption after backup rename restores the old installation', async t => {
    const { destination } = await sandbox(t);
    await mkdir(destination);
    await writeFile(join(destination, 'old'), 'old');
    const module = new URL('../src/install/transaction.ts', import.meta.url).href;
    const child = spawn(process.execPath, ['--input-type=module', '-e', `
        import {InstallationTransaction} from ${JSON.stringify(module)};
        import {mkdir,writeFile} from 'node:fs/promises';
        import {join} from 'node:path';
        const tx = new InstallationTransaction(process.argv[1]);
        await tx.open();
        const tree = join(tx.work,'new');
        await mkdir(tree); await writeFile(join(tree,'new'),'new');
        await tx.replace(tree, async () => { console.log('backed up'); await new Promise(() => {}); });
    `, destination], { stdio: ['ignore', 'pipe', 'pipe'] });
    await new Promise((done, reject) => {
        child.stdout.once('data', done);
        child.once('error', reject);
        child.once('exit', code => { if (code) reject(new Error(`child exited ${code}`)); });
    });
    const closed = new Promise(done => child.once('close', done));
    child.kill('SIGKILL');
    await closed;
    const tx = new InstallationTransaction(destination);
    for (let attempt = 0; ; attempt++) {
        try { await tx.open(); break; }
        catch (error) { if (attempt > 50 || !/lock/.test(error.message)) throw error; await new Promise(done => setTimeout(done, 10)); }
    }
    try {
        assert.equal(await readFile(join(destination, 'old'), 'utf8'), 'old');
        assert.equal(await exists(join(destination, 'new')), false);
    } finally { await tx.close(); }
});

test('fresh publication interrupted before commit is recoverable', async t => {
    const { destination } = await sandbox(t);
    const tx = new InstallationTransaction(destination);
    await mkdir(tx.work, { mode: 0o700 });
    await mkdir(destination);
    await writeFile(join(destination, 'uncommitted'), 'new');
    await writeFile(join(tx.work, 'transaction.json'), JSON.stringify({ hadDestination: false }));
    await tx.open();
    assert.equal(await exists(destination), false);
    await tx.close();
});

test('startup probe checks host OpenSSL libraries without development loader paths', async t => {
    const f = await fixture(t);
    const bin = join(f.root, 'host-bin');
    await mkdir(bin);
    const ldd = join(bin, 'ldd');
    const previous = { PATH: process.env.PATH, LD_LIBRARY_PATH: process.env.LD_LIBRARY_PATH };
    const output = 'libssl.so.3 => /usr/lib/libssl.so.3\nlibcrypto.so.3 => /usr/lib/libcrypto.so.3\n';
    await writeFile(ldd, `#!/bin/bash\n[[ -z "\${LD_LIBRARY_PATH+x}" ]] || exit 1\nprintf '%s' ${bashQuote(output)}\n`, { mode: 0o755 });
    process.env.PATH = `${bin}:${process.env.PATH}`;
    process.env.LD_LIBRARY_PATH = '/development-only';
    try {
        await smokeCheck(f.tree, 'linux-amd64-standard');
        await writeFile(ldd, '#!/bin/bash\nprintf "libssl.so.3 => not found\\n"\n', { mode: 0o755 });
        await assert.rejects(smokeCheck(f.tree, 'linux-amd64-standard'), /OpenSSL 3/);
    } finally {
        process.env.PATH = previous.PATH;
        if (previous.LD_LIBRARY_PATH === undefined) delete process.env.LD_LIBRARY_PATH;
        else process.env.LD_LIBRARY_PATH = previous.LD_LIBRARY_PATH;
    }
});

test('PATH editor preserves mixed line endings and refuses invalid UTF-8', async t => {
    const { home, destination } = await sandbox(t);
    const contents = '# first\r\n# second\n';
    assert.ok(editBashrc(contents, destination).startsWith(contents));
    await writeFile(join(home, '.bashrc'), Buffer.from([0xff, 0x0a]));
    await assert.rejects(updateBashrc(home, destination));
    assert.deepEqual(await readFile(join(home, '.bashrc')), Buffer.from([0xff, 0x0a]));
});

test('a failed rollback retains recovery files until restoration becomes possible', async t => {
    const { destination } = await sandbox(t);
    await mkdir(destination);
    await writeFile(join(destination, 'old'), 'old');
    const tx = new InstallationTransaction(destination);
    await tx.open();
    const tree = join(tx.work, 'new');
    await mkdir(tree);
    await writeFile(join(tree, 'new'), 'new');
    const previous = join(tx.preparation, 'previous');
    // Replace the backup slot with an invalid object after retaining the actual
    // old tree elsewhere, simulating a cleanup/restore failure deterministically.
    await assert.rejects(tx.replace(tree, async () => {
        await rename(previous, join(tx.work, 'saved-old'));
        await writeFile(previous, 'invalid backup');
        throw new Error('publication failed');
    }), /rollback failed.*recovery files retained/);
    await tx.close();
    assert.equal(await exists(join(tx.work, 'transaction.json')), true);
    assert.equal(await readFile(join(tx.work, 'saved-old/old'), 'utf8'), 'old');
    await rm(previous);
    await rename(join(tx.work, 'saved-old'), previous);
    const recovered = new InstallationTransaction(destination);
    await recovered.open();
    try { assert.equal(await readFile(join(destination, 'old'), 'utf8'), 'old'); }
    finally { await recovered.close(); }
});

test('cancelling streamed preparation leaves the old worker and releases its lock', async t => {
    const f = await fixture(t);
    await install(f);
    const controller = new AbortController();
    const fetcher = async url => {
        if (url === f.selected.checksumUrl) return new Response(f.checksum);
        return new Response(new ReadableStream({ start() {
            setImmediate(() => controller.abort(new Error('cancelled')));
        } }));
    };
    await assert.rejects(install(f, ['--reinstall'], { fetcher, signal: controller.signal }), /aborted|cancelled/);
    assert.equal(await exists(join(f.destination, 'bin/simplex')), true);
    const tx = new InstallationTransaction(f.destination);
    await tx.open();
    await tx.close();
});

test('committed cleanup failures warn without blocking PATH, no-op or later replacement', async t => {
    const f = await fixture(t);
    await install(f);
    await writeFile(join(f.destination, 'stale'), 'previous installation');
    let retained;
    let rejected = 0;
    const removeObsolete = async path => {
        if (!retained && await exists(join(path, 'previous'))) retained = path;
        if (path === retained) {
            rejected++;
            throw Object.assign(new Error('injected EACCES during old backup removal'), { code: 'EACCES' });
        }
        await rm(path, { recursive: true, force: true });
    };
    const replacement = await install(f, ['--reinstall', '--update-path'], { removeObsolete });
    assert.equal(replacement.code, 0);
    assert.match(replacement.output, /Installed worker 0.2.0 successfully/);
    assert.match(replacement.output, /Warning:.*retained at.*EACCES/);
    assert.ok(replacement.output.includes(retained));
    assert.match(replacement.output, /PATH block updated/);
    assert.match(replacement.output, /Manual PATH: export PATH=/);
    assert.match(replacement.output, /Verify:.*run --help/);
    assert.match(await readFile(join(f.home, '.bashrc'), 'utf8'), /managed worker PATH/);
    assert.equal(await exists(join(f.destination, 'stale')), false);
    assert.equal(await readFile(join(retained, 'previous/stale'), 'utf8'), 'previous installation');

    // Startup cleanup encounters the same error. A no-op must still inspect the
    // live tree and honor an explicit PATH update without downloading anything.
    await writeFile(join(f.home, '.bashrc'), '# retained user settings\n');
    const current = await install(f, ['--update-path'], {
        removeObsolete, fetcher: async () => { throw new Error('must not download current version'); },
    });
    assert.equal(current.code, 0);
    assert.match(current.output, /already current/);
    assert.match(current.output, /Warning:.*retained at/);
    assert.match(current.output, /PATH block updated/);
    assert.match(await readFile(join(f.home, '.bashrc'), 'utf8'), /^# retained user settings\n/);
    assert.equal(await exists(join(retained, 'previous/stale')), true);

    // A new transaction uses another backup/staging directory, so retained
    // debris cannot collide with the next publication or cause the old tree to win.
    await writeFile(join(f.destination, 'second-stale'), 'remove this');
    assert.equal((await install(f, ['--reinstall'], { removeObsolete })).code, 0);
    assert.equal(await exists(join(f.destination, 'second-stale')), false);
    assert.equal(await exists(join(retained, 'previous/stale')), true);
    assert.ok(rejected >= 3);
    assert.equal(await exists(join(new InstallationTransaction(f.destination).work, 'transaction.json')), false);
});

test('colon directories install but never produce a misleading PATH command', async t => {
    const f = await fixture(t);
    f.destination = join(f.home, 'worker:demo');
    await writeFile(join(f.home, '.bashrc'), '# unchanged\n');
    const installed = await install(f, ['--update-path']);
    assert.equal(installed.code, 1);
    assert.match(installed.output, /Worker is installed, but PATH update failed:.*PATH cannot represent/);
    assert.doesNotMatch(installed.output, /PATH block updated|export PATH=/);
    assert.match(installed.output, /Verify:.*worker:demo\/bin\/simplex.*run --help/);
    assert.match(installed.output, /launcher.command\[0\]/);
    assert.equal(await readFile(join(f.home, '.bashrc'), 'utf8'), '# unchanged\n');
    assert.equal(await exists(join(f.destination, 'bin/simplex')), true);
    const skipped = await install(f, ['--no-update-path']);
    assert.equal(skipped.code, 0);
    assert.doesNotMatch(skipped.output, /export PATH=/);
    assert.throws(() => editBashrc('', f.destination), /PATH cannot represent/);

    // Verify actual shell lookup: the existing PATH stays intact, and the
    // absolute worker command remains usable even though PATH cannot name it.
    const script = `source ${bashQuote(join(f.home, '.bashrc'))}\nprintf '%s\\n' "$PATH"\ncommand -v bash\n${bashQuote(join(f.destination, 'bin/simplex'))} run --help`;
    const { stdout } = await exec('bash', ['--noprofile', '--norc', '-c', script], {
        env: { PATH: '/usr/bin:/bin', HOME: f.home },
    });
    assert.equal(stdout.split('\n')[0], '/usr/bin:/bin');
    assert.match(stdout, /\/usr\/bin\/bash\nworker help/);
});

test('managed shell blocks use LF and preserve the exact PATH with CRLF input', async t => {
    const { root } = await sandbox(t);
    const contents = '# existing comment\r\n# second comment\r\n';
    const edited = editBashrc(contents, '/installed/worker');
    assert.ok(edited.startsWith(contents));
    const block = edited.slice(contents.length);
    assert.doesNotMatch(block, /\r/);
    const old = `${contents}${START}\r\nexport PATH='/old/bin':"$PATH"\r\n${END}\r\n# tail\r\n`;
    const repaired = editBashrc(old, '/installed/worker');
    assert.ok(repaired.startsWith(contents));
    assert.ok(repaired.endsWith('# tail\r\n'));
    assert.equal(editBashrc(repaired, '/installed/worker'), repaired);
    for (const text of [edited, repaired]) {
        const { stdout } = await exec('bash', ['--noprofile', '--norc', '-c', `${text}printf '%s' "$PATH"`], {
            env: { PATH: '/usr/bin:/bin', HOME: root },
        });
        assert.equal(stdout, '/installed/worker/bin:/usr/bin:/bin');
        assert.deepEqual(stdout.split(':'), ['/installed/worker/bin', '/usr/bin', '/bin']);
        assert.doesNotMatch(stdout, /\r/);
    }
});

test('recovery rejects an invalid attempt path and preserves the live tree and journal', async t => {
    const { destination } = await sandbox(t);
    await mkdir(destination);
    await writeFile(join(destination, 'keep'), 'keep');
    const tx = new InstallationTransaction(destination);
    await mkdir(tx.work, { mode: 0o700 });
    for (const attempt of ['../other', '/absolute', 'attempt-not-a-uuid', 42]) {
        await writeFile(join(tx.work, 'transaction.json'), JSON.stringify({ hadDestination: true, attempt }));
        await assert.rejects(new InstallationTransaction(destination).open(), /Invalid installation recovery journal/);
        assert.equal(await readFile(join(destination, 'keep'), 'utf8'), 'keep');
        assert.equal(await exists(join(tx.work, 'transaction.json')), true);
    }
});

test('installer keeps selecting glibc from a release manifest containing both runtimes', async t => {
    const f = await fixture(t);
    const manifest = '0'.repeat(64) + '  ' + f.selected.archive.replace('-glibc2.34', '-musl') + '\n' + f.checksum;
    const result = await install(f, [], {
        fetcher: async url => new Response(url === f.selected.checksumUrl ? manifest : f.bytes),
    });
    assert.equal(result.code, 0, result.output);
    assert.match(await readFile(join(f.destination, 'bin/simplex_worker'), 'utf8'), /worker help/);
});

test('platform is mandatory, help lists semantic tags, and invalid selection has no side effects', async t => {
    assert.throws(() => parseInstallArguments([]), /--platform is required/);
    assert.throws(() => parseInstallArguments(['--platform']), /requires a value/);
    for (const tag of ['linux-amd64-musl', 'linux-amd64-glibc', 'linux-arm64-bundled', '__proto__']) {
        assert.throws(() => parseInstallArguments(['--platform', tag]), /Unsupported platform/);
    }
    assert.throws(() => parseInstallArguments(['--platform', 'linux-amd64-standard',
        '--platform', 'linux-amd64-bundled']), /specified once/);
    assert.equal(parseInstallArguments(['--help']).help, true);
    const f = await fixture(t);
    let touched = false;
    await assert.rejects(installWorker(['--directory', f.destination], {
        home: f.home, checkHost: async () => { touched = true; },
    }), /--platform is required/);
    assert.equal(touched, false);
    assert.deepEqual(await readdir(f.home), []);
    const cli = resolve(import.meta.dirname, '../bin/simplex-hub.ts');
    const help = (await exec(process.execPath, [cli, 'install-worker', '--help'])).stdout;
    for (const tag of Object.keys(WORKER_PLATFORMS)) assert.ok(help.includes(tag));
    assert.match(help, /host glibc >= 2.34 and OpenSSL 3/);
    assert.match(help, /bundles the musl loader, OpenSSL/);
});

test('each selected platform resolves exactly its own official asset without fallback', async () => {
    const url = name => `https://github.com/Hazer-BJTU/simplex-cpp/releases/download/v0.5.4/${name}`;
    const document = { draft: false, prerelease: false, tag_name: 'v0.5.4', assets:
        [...Object.keys(WORKER_PLATFORMS).map(tag => release('0.5.4', tag).archive), 'SHA256SUMS']
            .map(name => ({ name, browser_download_url: url(name) })) };
    const fetcher = async () => new Response(JSON.stringify(document));
    for (const platform of Object.keys(WORKER_PLATFORMS)) {
        const selected = await resolveRelease('github', platform, 'v0.5.4', fetcher);
        assert.equal(selected.platform, platform);
        assert.equal(selected.archive, release('0.5.4', platform).archive);
        assert.equal(selected.archiveUrl, url(selected.archive));
    }
    const bundled = release('0.5.4', 'linux-amd64-bundled');
    document.assets = document.assets.filter(asset => asset.name !== bundled.archive);
    await assert.rejects(resolveRelease('github', bundled.platform, undefined, fetcher), /must contain one .*musl/);
    document.assets.push({ name: bundled.archive, browser_download_url: url(bundled.archive).replace('github.com', 'example.com') });
    await assert.rejects(resolveRelease('github', bundled.platform, undefined, fetcher), /Unexpected official/);
    document.assets.push({ name: bundled.archive, browser_download_url: url(bundled.archive) });
    await assert.rejects(resolveRelease('github', bundled.platform, undefined, fetcher), /must contain one/);
});

test('bundled packages do not need host glibc or ldd but still require Linux amd64', async t => {
    assert.doesNotThrow(() => validatePlatform('linux-amd64-bundled', 'linux', 'x64', undefined));
    assert.doesNotThrow(() => validatePlatform('linux-amd64-bundled', 'linux', 'x64', '2.17'));
    for (const [os, arch] of [['darwin', 'x64'], ['linux', 'arm64']]) {
        assert.throws(() => validatePlatform('linux-amd64-bundled', os, arch, undefined), /Linux x86_64/);
    }
    const { root } = await sandbox(t);
    const marker = join(root, 'ldd-was-called');
    await writeFile(join(root, 'ldd'), `#!/bin/bash\ntouch ${bashQuote(marker)}\nexit 1\n`, { mode: 0o755 });
    const oldPath = process.env.PATH;
    process.env.PATH = root + ':' + oldPath;
    try {
        await validateHost('linux-amd64-bundled');
        assert.equal(await exists(marker), false);
        await assert.rejects(validateHost('linux-amd64-standard'), /ldd/);
        assert.equal(await exists(marker), true);
    } finally {
        process.env.PATH = oldPath;
    }
});

test('bundled installation uses the complete wrapper layout and supports spaces, relocation and PATH', async t => {
    const f = await fixture(t, { version: '0.5.4', platform: 'linux-amd64-bundled' });
    f.destination = join(f.home, 'worker directory 中文');
    assert.equal((await install(f, ['--update-path'])).code, 0);
    assert.equal((await readInstalled(f.destination)).platform, 'linux-amd64-bundled');
    assert.equal((await lstat(join(f.destination, 'bin/simplex_worker.bin'))).mode & 0o111, 0o111);
    assert.match(await readFile(join(f.destination, 'bin/simplex_worker'), 'utf8'), /exec "\$loader"/);
    assert.equal(await readFile(join(f.destination, 'share/certs/ca-certificates.crt'), 'utf8'), 'fixture CA');
    assert.equal(await readFile(join(f.destination, 'share/openssl/openssl.cnf'), 'utf8'), 'fixture OpenSSL config');
    assert.ok((await readFile(join(f.home, '.bashrc'), 'utf8')).includes(f.destination + '/bin'));
    const moved = join(f.home, 'relocated worker 中文');
    await rename(f.destination, moved);
    await smokeCheck(moved, 'linux-amd64-bundled');
    const current = await install({ ...f, destination: moved }, [], {
        fetcher: async () => { throw new Error('current bundle must not redownload'); },
    });
    assert.match(current.output, /already current/);
    await rm(join(moved, 'lib/ld-musl-x86_64.so.1'));
    await assert.rejects(install({ ...f, destination: moved }), /Current installation is incomplete/);
});

test('platform switches replace same-version trees and preserve independent downgrade authorization', async t => {
    const standard = await fixture(t);
    await install(standard);
    const metadata = JSON.parse(await readFile(join(standard.destination, METADATA), 'utf8'));
    delete metadata.platform;
    await writeFile(join(standard.destination, METADATA), JSON.stringify(metadata));
    assert.equal((await readInstalled(standard.destination)).platform, 'linux-amd64-standard');
    assert.match((await install(standard, [], { fetcher: async () => { throw new Error('legacy no-op'); } })).output, /already current/);
    const bundled = await fixture(t, { platform: 'linux-amd64-bundled' });
    const target = { ...bundled, home: standard.home, destination: standard.destination };
    await install(target);
    assert.equal((await readInstalled(target.destination)).platform, bundled.selected.platform);
    assert.equal(await exists(join(target.destination, 'bin/simplex_worker.bin')), true);
    await install(standard);
    assert.equal((await readInstalled(target.destination)).platform, standard.selected.platform);
    assert.equal(await exists(join(target.destination, 'bin/simplex_worker.bin')), false);
    const newer = await fixture(t, { version: '0.5.4', platform: 'linux-amd64-bundled' });
    await install({ ...newer, home: standard.home, destination: standard.destination });
    await assert.rejects(install(standard, ['--overwrite', '--reinstall']), /allow-downgrade/);
    await install(standard, ['--allow-downgrade']);
    metadata.platform = 'unknown-package';
    await writeFile(join(standard.destination, METADATA), JSON.stringify(metadata));
    assert.equal(await readInstalled(standard.destination), null);
    await assert.rejects(install(standard), /--overwrite/);
});

test('broken bundled runtime validation leaves the current standard installation intact', async t => {
    const standard = await fixture(t);
    await install(standard);
    const bundle = await fixture(t, { platform: 'linux-amd64-bundled', broken: true });
    await assert.rejects(install({ ...bundle, home: standard.home, destination: standard.destination }), /Not executable/);
    assert.equal((await readInstalled(standard.destination)).platform, standard.selected.platform);
    for (const file of ['bin/simplex_worker.bin', 'lib/ld-musl-x86_64.so.1', 'lib/libssl.so.3', 'lib/libcrypto.so.3',
        'share/certs/ca-certificates.crt', 'share/openssl/openssl.cnf']) {
        const valid = await fixture(t, { platform: 'linux-amd64-bundled' });
        await rm(join(valid.tree, file));
        await assert.rejects(validateTree(valid.tree, valid.selected.platform));
    }
});

test('bundled startup uses only shipped dependencies and clears development injection variables', async t => {
    const f = await fixture(t, { platform: 'linux-amd64-bundled' });
    const previous = { LD_LIBRARY_PATH: process.env.LD_LIBRARY_PATH, LD_PRELOAD: process.env.LD_PRELOAD,
        LD_AUDIT: process.env.LD_AUDIT };
    process.env.LD_LIBRARY_PATH = '/development-only';
    process.env.LD_PRELOAD = '/development-only/preload.so';
    process.env.LD_AUDIT = '/development-only/audit.so';
    try {
        await smokeCheck(f.tree, f.selected.platform);
    } finally {
        for (const [name, value] of Object.entries(previous)) {
            if (value === undefined) delete process.env[name]; else process.env[name] = value;
        }
    }
    const loader = join(f.tree, 'lib/ld-musl-x86_64.so.1');
    await writeFile(loader, '#!/bin/bash\nprintf "libssl.so.3 => /host/libssl.so.3 (0x1)\\n"\n', { mode: 0o755 });
    await assert.rejects(smokeCheck(f.tree, f.selected.platform), /outside the package/);
    await writeFile(loader, '#!/bin/bash\nprintf "/lib/ld-musl-x86_64.so.1 (0x1)\\nlibc.musl-x86_64.so.1 => /lib/ld-musl-x86_64.so.1 (0x2)\\n"\n', { mode: 0o755 });
    await assert.rejects(smokeCheck(f.tree, f.selected.platform), /outside the package/);
    await writeFile(loader, '#!/bin/bash\nexit 127\n', { mode: 0o755 });
    await assert.rejects(smokeCheck(f.tree, f.selected.platform), /bundled loader\/runtime/);
});

test('bundled preparation selects its exact checksum from a multi-platform manifest', async t => {
    const f = await fixture(t, { platform: 'linux-amd64-bundled' });
    const standard = '0'.repeat(64) + '  ' + release().archive + '\n';
    const work = join(f.root, 'work');
    await mkdir(work);
    const tree = await prepareArchive(f.selected, work,
        async url => new Response(url === f.selected.checksumUrl ? standard + f.checksum : f.bytes));
    await smokeCheck(tree, f.selected.platform);
    const another = join(f.root, 'another');
    await mkdir(another);
    await assert.rejects(prepareArchive(f.selected, another,
        async url => new Response(url === f.selected.checksumUrl ? standard : f.bytes)), /exactly one checksum/);
});

test('bundled colon paths fail before release resolution and workspace creation', async t => {
    const f = await fixture(t, { platform: 'linux-amd64-bundled' });
    f.destination = join(f.home, 'colon:worker');
    await assert.rejects(install(f, [], { release: async () => { throw new Error('must not resolve'); } }), /loader search-path separator/);
    assert.deepEqual(await readdir(f.home), []);
});
