#!/usr/bin/env node
/** Refuse to publish artifacts from an unrelated or untested workflow run. */
import { execFileSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';

const tag = process.env.RELEASE_TAG;
const runId = process.env.SOURCE_RUN_ID;
const repo = process.env.GITHUB_REPOSITORY;
const token = process.env.GH_TOKEN;
if (!/^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(tag || '') ||
    !/^[1-9]\d*$/.test(runId || '') || !repo || !token) {
    throw new Error('Recovery needs a version tag, numeric source run ID, repository, and GH_TOKEN');
}

const version = (await readFile(new URL('../VERSION', import.meta.url), 'utf8')).trim();
if (tag !== `v${version}`) throw new Error(`Release tag ${tag} does not match VERSION ${version}`);
const git = args => execFileSync('git', args, { encoding: 'utf8' }).trim();
const tagCommit = git(['rev-list', '-n', '1', tag]);
git(['merge-base', '--is-ancestor', tagCommit, 'HEAD']);

// The artifacts belong to the tagged source. Only release machinery may have
// changed between that source and the manual recovery workflow on main.
const allowedChanges = new Set([
    '.github/workflows/release-worker.yml',
    'docker/release-worker.mjs',
    'docker/verify-release-recovery.mjs',
    'hub/scripts/publish-release.mjs',
    'hub/test/release-worker.test.js',
    'hub/test/publish-release.test.js',
    'hub/docs/npm-release.md',
]);
const changes = git(['diff', '--name-only', tagCommit, 'HEAD']);
for (const path of changes ? changes.split('\n') : []) {
    if (!allowedChanges.has(path)) {
        throw new Error(`Application changed since ${tag}: ${path}`);
    }
}

async function get(path) {
    const url = `${process.env.GITHUB_API_URL || 'https://api.github.com'}/repos/${repo}/${path}`;
    const response = await fetch(url, {
        headers: {
            Authorization: `Bearer ${token}`,
            Accept: 'application/vnd.github+json',
            'X-GitHub-Api-Version': '2022-11-28',
        },
    });
    if (!response.ok) throw new Error(`GitHub API ${path} returned HTTP ${response.status}`);
    return response.json();
}

const run = await get(`actions/runs/${runId}`);
if (run.event !== 'push' || run.head_branch !== tag ||
    run.head_sha !== tagCommit || run.path !== '.github/workflows/release-worker.yml' ||
    run.status !== 'completed') {
    throw new Error(`Run ${runId} is not a completed tag workflow for ${tagCommit}`);
}

const required = new Set([
    'validate-tag',
    'build',
    'build-hub',
    'target-test (ubuntu:22.04)',
    'target-test (almalinux:9)',
]);
for (let page = 1; required.size; page++) {
    const result = await get(`actions/runs/${runId}/jobs?per_page=100&page=${page}`);
    if (!Array.isArray(result.jobs)) throw new Error('Source run has no job list');
    for (const job of result.jobs) {
        if (required.has(job.name)) {
            if (job.conclusion !== 'success') {
                throw new Error(`Source run job ${job.name} did not pass`);
            }
            required.delete(job.name);
        }
    }
    if (result.jobs.length < 100) break;
}
if (required.size) throw new Error(`Source run is missing passing jobs: ${[...required].join(', ')}`);
console.log(`Verified ${tag} artifacts from workflow run ${runId}`);
