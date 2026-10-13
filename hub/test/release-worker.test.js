import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { chmodSync, mkdtempSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn, spawnSync } from 'node:child_process';
import { after, test } from 'node:test';
import { parse } from 'yaml';
import { assembleWorkerAssets, validateWorkerAssets, workerArchives } from '../../docker/worker-release-assets.mjs';

const directory = mkdtempSync(join(tmpdir(), 'simplex-gh-test-'));
after(() => rmSync(directory, { recursive: true, force: true }));

test('publish and recovery runners populate locked runtime dependencies before offline artifact validation', () => {
    const workflow = parse(readFileSync(new URL('../../.github/workflows/release-worker.yml', import.meta.url), 'utf8'));
    for (const job of ['publish-hub', 'recover']) {
        const steps = workflow.jobs[job].steps;
        const validation = steps.findIndex(step => step.run?.includes('scripts/check-release-package.mjs'));
        const preparation = steps.findIndex(step => /^npm ci\b/m.test(step.run ?? ''));
        assert.ok(preparation >= 0 && preparation < validation,
            `${job} must prepare its own dependency cache before validating the archive`);
        const step = steps[preparation];
        assert.equal(step['working-directory'], 'hub');
        assert.match(step.run, /--omit=dev\b/);
        assert.match(step.run, /--ignore-scripts\b/);
        assert.doesNotMatch(step.run, /--offline\b/);
    }
});

test('publish and recovery reject zero or multiple archives before invoking artifact validators', () => {
    const workflow = parse(readFileSync(new URL('../../.github/workflows/release-worker.yml', import.meta.url), 'utf8'));
    for (const job of ['publish-hub', 'recover']) {
        const steps = workflow.jobs[job].steps;
        const run = steps.find(step => step.run?.includes('scripts/publish-release.mjs')).run;
        const validator = run.indexOf('node scripts/check-release-package.mjs');
        assert.ok(validator > 0);
        const admission = run.slice(0, validator);
        const root = mkdtempSync(join(directory, `${job}-archives-`));
        mkdirSync(join(root, 'hub-release'));
        function admitted() {
            return spawnSync('bash', ['-e', '-c', admission + '\nprintf "%s" "${archive[0]}"'],
                { cwd: root, encoding: 'utf8' });
        }
        assert.notEqual(admitted().status, 0, `${job}: empty glob must fail at admission`);
        writeFileSync(join(root, 'hub-release/one.tgz'), 'one');
        const single = admitted();
        assert.equal(single.status, 0);
        assert.equal(single.stdout, 'hub-release/one.tgz');
        writeFileSync(join(root, 'hub-release/two.tgz'), 'two');
        assert.notEqual(admitted().status, 0, `${job}: ambiguous archive must fail at admission`);
    }
});

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
    const muslArchive = 'simplex-worker-v0.1.0-linux-x86_64-musl.tar.gz';
    writeFileSync(join(assets, muslArchive), bytes);
    writeFileSync(join(assets, 'SHA256SUMS'), [archive, muslArchive].map(name =>
        `${createHash('sha256').update(bytes).digest('hex')}  ${name}\n`).join(''));
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
    let hiddenDraftPolls = 2;
    let hiddenAssetPolls = 2;
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
            if (hiddenDraftPolls > 0) {
                hiddenDraftPolls--;
                response.end('[]');
            } else {
                if (state.assets.length && hiddenAssetPolls > 0) {
                    hiddenAssetPolls--;
                    response.end(JSON.stringify([{ ...state, assets: [] }]));
                } else {
                    response.end(JSON.stringify([state]));
                }
            }
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
    interrupted.assets = interrupted.assets.filter(asset => asset.name !== muslArchive);
    writeFileSync(stateFile, JSON.stringify(interrupted));
    assert.equal((await run(script, { ...env, MODE: 'prepare' })).code, 0);
    assert.equal((await run(script, { ...env, MODE: 'finalize' })).code, 0);
    assert.equal((await run(script, { ...env, MODE: 'finalize' })).code, 0);
    assert.equal(JSON.parse(readFileSync(stateFile)).draft, false);
    assert.equal(JSON.parse(readFileSync(stateFile)).assets.length, 3);
    writeFileSync(join(remote, muslArchive), 'corrupted');
    const result = await run(script, { ...env, MODE: 'prepare' });
    assert.notEqual(result.code, 0);
    assert.match(result.output, /Existing release asset differs/);
});

test('release assembly waits for both validated variants before publication', () => {
    const workflow = parse(readFileSync(new URL('../../.github/workflows/release-worker.yml', import.meta.url), 'utf8'));
    assert.equal(workflow.jobs.musl.uses, './.github/workflows/musl-portable.yml');
    assert.equal(workflow.jobs.musl.needs, 'validate-tag');
    assert.deepEqual(workflow.jobs['assemble-worker-assets'].needs, ['target-test', 'musl']);
    assert.equal(workflow.jobs['assemble-worker-assets'].if, undefined);
    assert.equal(workflow.jobs.musl['continue-on-error'], undefined);
    assert.deepEqual(workflow.jobs['prepare-release'].needs, ['assemble-worker-assets', 'build-hub']);
});

test('assembly excludes test payloads and rejects corrupted, missing or stale release assets', async () => {
    const root = mkdtempSync(join(directory, 'assembly-'));
    const glibc = join(root, 'glibc');
    const musl = join(root, 'musl');
    const output = join(root, 'output');
    mkdirSync(glibc);
    mkdirSync(musl);
    const tag = 'v0.1.0';
    const [glibcName, muslName] = workerArchives(tag);
    for (const [input, name, manifest] of [
        [glibc, glibcName, 'SHA256SUMS'], [musl, muslName, muslName + '.sha256'],
    ]) {
        writeFileSync(join(input, name), name);
        writeFileSync(join(input, manifest),
            `${createHash('sha256').update(name).digest('hex')}  ${name}\n`);
    }
    writeFileSync(join(musl, 'musl-test-support.tar.gz'), 'not distributable');
    writeFileSync(join(musl, 'musl-launch-probe'), 'not distributable');
    await assembleWorkerAssets(glibc, musl, output, tag);
    assert.deepEqual(readdirSync(output).sort(), ['SHA256SUMS', glibcName, muslName].sort());
    await validateWorkerAssets(output, tag);
    await assert.rejects(assembleWorkerAssets(glibc, musl, output, tag), /EEXIST/);
    await assert.rejects(validateWorkerAssets(output, 'v0.1.1'), /Expected exactly/);
    writeFileSync(join(output, 'probe'), 'unexpected');
    await assert.rejects(validateWorkerAssets(output, tag), /Expected exactly/);
    rmSync(join(output, 'probe'));
    writeFileSync(join(output, muslName), 'corrupted');
    await assert.rejects(validateWorkerAssets(output, tag), /Checksum mismatch/);
    rmSync(join(output, muslName));
    await assert.rejects(validateWorkerAssets(output, tag), /Expected exactly/);
    writeFileSync(join(musl, muslName), 'corrupted');
    await assert.rejects(assembleWorkerAssets(glibc, musl, join(root, 'bad'), tag), /Checksum mismatch/);
});

test('recovery requires successful assembly as the musl validation fence', async t => {
    const root = mkdtempSync(join(directory, 'recovery-'));
    const git = join(root, 'git');
    writeFileSync(git, '#!/bin/sh\nif [ "$1" = rev-list ]; then printf tagcommit; fi\n');
    chmodSync(git, 0o755);
    const tag = 'v' + readFileSync(new URL('../../VERSION', import.meta.url), 'utf8').trim();
    const jobs = ['validate-tag', 'build', 'build-hub', 'target-test (ubuntu:22.04)',
        'target-test (almalinux:9)'].map(name => ({ name, conclusion: 'success' }));
    let assembly;
    const server = createServer((request, response) => {
        response.setHeader('Content-Type', 'application/json');
        response.end(JSON.stringify(request.url.includes('/jobs?')
            ? { jobs: assembly ? [...jobs, assembly] : jobs }
            : { event: 'push', head_branch: tag, head_sha: 'tagcommit',
                path: '.github/workflows/release-worker.yml', status: 'completed' }));
    });
    await new Promise(done => server.listen(0, '127.0.0.1', done));
    t.after(() => new Promise(done => server.close(done)));
    const env = { ...process.env, RELEASE_TAG: tag, SOURCE_RUN_ID: '123',
        GITHUB_REPOSITORY: 'example/simplex', GH_TOKEN: 'fake',
        GITHUB_API_URL: `http://127.0.0.1:${server.address().port}`,
        PATH: `${root}:${process.env.PATH}`, MODE: '', ASSETS: '' };
    const script = fileURLToPath(new URL('../../docker/verify-release-recovery.mjs', import.meta.url));
    assert.match((await run(script, env)).output, /missing passing jobs: assemble-worker-assets/);
    assembly = { name: 'assemble-worker-assets', conclusion: 'failure' };
    assert.match((await run(script, env)).output, /assemble-worker-assets did not pass/);
    assembly.conclusion = 'success';
    const result = await run(script, env);
    assert.equal(result.code, 0, result.output);
});
