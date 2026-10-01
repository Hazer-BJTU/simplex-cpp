#!/usr/bin/env node
/** Prepare/recover a draft GitHub release, then finalize it after npm succeeds. */
import { execFileSync } from 'node:child_process';
import { mkdtemp, readFile, readdir, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, join, resolve } from 'node:path';

const [mode, assetDirectory] = process.argv.slice(2);
const tag = process.env.RELEASE_TAG || process.env.GITHUB_REF_NAME;
const repo = process.env.GITHUB_REPOSITORY;
const token = process.env.GH_TOKEN;
if (!['prepare', 'finalize'].includes(mode) || !assetDirectory || !tag || !repo || !token) {
    throw new Error('Usage: GITHUB_REF_NAME=... GITHUB_REPOSITORY=... GH_TOKEN=... node docker/release-worker.mjs prepare|finalize ASSET_DIR');
}

const directory = resolve(assetDirectory);
const archives = (await readdir(directory)).filter(name =>
    /^simplex-worker-v[0-9]+\.[0-9]+\.[0-9]+-linux-x86_64-glibc2\.34\.tar\.gz$/.test(name));
if (archives.length !== 1) throw new Error('Expected exactly one worker archive');
if (!archives[0].startsWith(`simplex-worker-${tag}-`)) {
    throw new Error(`Worker archive does not match release tag ${tag}`);
}
const names = [archives[0], 'SHA256SUMS'];
const checksum = (await readFile(join(directory, 'SHA256SUMS'), 'utf8')).trim();
if (!new RegExp(`^[a-f0-9]{64}  ${archives[0].replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}$`).test(checksum)) {
    throw new Error('SHA256SUMS must identify exactly the release archive');
}
execFileSync('sha256sum', ['-c', 'SHA256SUMS'], { cwd: directory, stdio: 'inherit' });

function gh(args) {
    execFileSync('gh', args, { stdio: 'inherit' });
}

async function release() {
    // GitHub's get-by-tag REST endpoint returns 404 for unpublished drafts.
    // The authenticated release list includes drafts, so use it for both
    // preparation and finalization of the same release.
    for (let page = 1; ; page++) {
        const url = `${process.env.GITHUB_API_URL || 'https://api.github.com'}` +
            `/repos/${repo}/releases?per_page=100&page=${page}`;
        const response = await fetch(url, {
            headers: {
                Authorization: `Bearer ${token}`,
                Accept: 'application/vnd.github+json',
                'X-GitHub-Api-Version': '2022-11-28',
            },
        });
        if (!response.ok) throw new Error(`GitHub release lookup failed: HTTP ${response.status}`);
        const releases = await response.json();
        if (!Array.isArray(releases)) throw new Error('GitHub release list is not an array');
        const match = releases.find(item => item.tag_name === tag);
        if (match) return match;
        if (releases.length < 100) return null;
    }
}

async function waitFor(check) {
    for (let attempt = 0; attempt < 10; attempt++) {
        const current = await release();
        if (current && await check(current)) return current;
        if (attempt < 9) await new Promise(resolve => setTimeout(resolve, 2000));
    }
    return null;
}

async function verifyAssets(current, allowMissing) {
    const remote = current.assets.filter(asset => names.includes(asset.name));
    for (const name of names) {
        const matches = remote.filter(asset => asset.name === name);
        if (matches.length > 1) throw new Error(`Duplicate release asset: ${name}`);
        if (!matches.length) {
            if (!allowMissing) throw new Error(`Published release is missing ${name}`);
            continue;
        }
        const temporary = await mkdtemp(join(tmpdir(), 'simplex-release-'));
        try {
            gh(['release', 'download', tag, '--repo', repo, '--dir', temporary, '--pattern', name]);
            const actual = await readFile(join(temporary, basename(name)));
            const expected = await readFile(join(directory, name));
            if (!actual.equals(expected)) throw new Error(`Existing release asset differs: ${name}`);
        } finally {
            await rm(temporary, { recursive: true, force: true });
        }
    }
    return names.filter(name => !remote.some(asset => asset.name === name));
}

let current = await release();
if (mode === 'prepare') {
    if (!current) {
        const notes = `Source snapshots are attached automatically by GitHub. The worker asset is for Linux x86_64 (glibc 2.34+, host OpenSSL 3); see README.md inside the archive. The Hub is available separately from npm as @hazer-bjtu/simplex-hub@${tag.slice(1)}.`;
        try {
            gh(['release', 'create', tag, '--repo', repo, '--verify-tag', '--draft',
                '--title', `Simplex ${tag}`, '--notes', notes]);
        } catch (error) {
            // A retry or concurrent run may have created the draft already.
            if (!(await waitFor(() => true))) throw error;
        }
        current = await waitFor(() => true);
    }
    if (!current) throw new Error('GitHub draft release was not created');
    const missing = await verifyAssets(current, current.draft);
    if (missing.length && !current.draft) throw new Error('Published release has incomplete assets');
    for (const name of missing) {
        try {
            gh(['release', 'upload', tag, join(directory, name), '--repo', repo]);
        } catch (error) {
            // Verify the server state before deciding whether the upload failed.
            current = await waitFor(async item =>
                !(await verifyAssets(item, true)).includes(name));
            if (!current) throw error;
        }
    }
    current = await waitFor(async item =>
        (await verifyAssets(item, true)).length === 0);
    if (!current) throw new Error('GitHub release assets were not visible after upload');
    console.log('Verified GitHub release assets');
} else {
    if (!current) throw new Error('GitHub draft release is missing');
    await verifyAssets(current, false);
    if (current.draft) {
        try {
            gh(['release', 'edit', tag, '--repo', repo, '--draft=false']);
        } catch (error) {
            if (!(await waitFor(item => item.draft === false))) throw error;
        }
    }
    current = await waitFor(item => item.draft === false);
    if (!current) throw new Error('GitHub release is still a draft');
    await verifyAssets(current, false);
    console.log('Verified published GitHub release');
}
