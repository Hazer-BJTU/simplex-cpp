import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { chmodSync, copyFileSync, mkdtempSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
import { after, test } from 'node:test';

const directory = mkdtempSync(join(tmpdir(), 'simplex-gh-test-'));
after(() => rmSync(directory, { recursive: true, force: true }));

function run(script, env) {
    return new Promise(resolve => {
        const child = spawn(process.execPath, [script, env.MODE, env.ASSETS], {
            env, stdio: ['ignore', 'pipe', 'pipe'],
        });
        let output = '';
        child.stdout.on('data', data => { output += data; });
        child.stderr.on('data', data => { output += data; });
        child.on('close', code => resolve({ code, output }));
    });
}

test('GitHub release can resume after draft, upload, and publication', async t => {
    const assets = join(directory, 'assets');
    const remote = join(directory, 'remote');
    const bin = join(directory, 'bin');
    mkdirSync(assets);
    mkdirSync(remote);
    mkdirSync(bin);
    const archive = 'simplex-worker-v0.1.0-linux-x86_64-glibc2.34.tar.gz';
    const bytes = Buffer.from('tested worker archive');
    writeFileSync(join(assets, archive), bytes);
    writeFileSync(join(assets, 'SHA256SUMS'),
        `${createHash('sha256').update(bytes).digest('hex')}  ${archive}\n`);
    const stateFile = join(directory, 'state.json');
    const gh = join(bin, 'gh');
    writeFileSync(gh, `#!/usr/bin/env node
const fs = require('node:fs');
const path = require('node:path');
const stateFile = process.env.FAKE_GH_STATE;
const remote = process.env.FAKE_GH_REMOTE;
const args = process.argv.slice(2);
let state = fs.existsSync(stateFile) ? JSON.parse(fs.readFileSync(stateFile)) : null;
const command = args[1];
if (command === 'create') state = { tag_name: 'v0.1.0', draft: true, assets: [] };
else if (command === 'upload') {
    const name = path.basename(args[3]);
    fs.copyFileSync(args[3], path.join(remote, name));
    state.assets.push({ name });
} else if (command === 'download') {
    const name = args[args.indexOf('--pattern') + 1];
    const target = args[args.indexOf('--dir') + 1];
    fs.copyFileSync(path.join(remote, name), path.join(target, name));
} else if (command === 'edit') state.draft = false;
else process.exit(2);
fs.writeFileSync(stateFile, JSON.stringify(state));
`);
    chmodSync(gh, 0o755);
    const server = createServer((request, response) => {
        // GitHub's get-by-tag endpoint cannot see a draft release. The
        // release script must use the authenticated list instead.
        if (request.url !== '/repos/example/simplex/releases?per_page=100&page=1') {
            response.writeHead(404).end();
            return;
        }
        try {
            const state = JSON.parse(readFileSync(stateFile, 'utf8'));
            response.setHeader('Content-Type', 'application/json');
            response.end(JSON.stringify([state]));
        } catch {
            response.setHeader('Content-Type', 'application/json');
            response.end('[]');
        }
    });
    await new Promise(done => server.listen(0, '127.0.0.1', done));
    t.after(() => new Promise(done => server.close(done)));
    const env = {
        ...process.env,
        GITHUB_REF_NAME: 'v0.1.0',
        GITHUB_REPOSITORY: 'example/simplex',
        GITHUB_API_URL: `http://127.0.0.1:${server.address().port}`,
        GH_TOKEN: 'test-token',
        FAKE_GH_STATE: stateFile,
        FAKE_GH_REMOTE: remote,
        PATH: `${bin}:${process.env.PATH}`,
        ASSETS: assets,
    };
    const script = fileURLToPath(new URL('../../docker/release-worker.mjs', import.meta.url));
    assert.equal((await run(script, { ...env, MODE: 'prepare' })).code, 0);
    // Simulate an interrupted asset upload on the draft.
    const interrupted = JSON.parse(readFileSync(stateFile));
    interrupted.assets = interrupted.assets.filter(asset => asset.name !== 'SHA256SUMS');
    writeFileSync(stateFile, JSON.stringify(interrupted));
    assert.equal((await run(script, { ...env, MODE: 'prepare' })).code, 0);
    assert.equal((await run(script, { ...env, MODE: 'finalize' })).code, 0);
    assert.equal((await run(script, { ...env, MODE: 'finalize' })).code, 0);
    assert.equal(JSON.parse(readFileSync(stateFile)).draft, false);
    copyFileSync(join(assets, archive), join(remote, archive));
    writeFileSync(join(remote, archive), 'corrupted');
    const result = await run(script, { ...env, MODE: 'prepare' });
    assert.notEqual(result.code, 0);
    assert.match(result.output, /Existing release asset differs/);
});
