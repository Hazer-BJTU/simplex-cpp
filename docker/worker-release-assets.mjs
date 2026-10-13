#!/usr/bin/env node
/** Assemble tested runtime variants without publishing validation-only files. */
import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { copyFile, lstat, mkdir, readFile, readdir, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

export function workerArchives(tag) {
    if (!/^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/.test(tag ?? '')) {
        throw new Error('Expected a stable version tag');
    }
    return ['glibc2.34', 'musl'].map(runtime =>
        `simplex-worker-${tag}-linux-x86_64-${runtime}.tar.gz`);
}

async function regularFile(path) {
    if (!(await lstat(path)).isFile()) throw new Error(`Not a regular release file: ${path}`);
}

/** Check an exact manifest before interpreting filenames or copying bytes. */
async function verifyManifest(directory, manifest, expected) {
    await regularFile(join(directory, manifest));
    const lines = (await readFile(join(directory, manifest), 'utf8')).trim().split('\n');
    const entries = lines.map(line => /^([a-f0-9]{64})  ([^\r\n]+)$/.exec(line));
    const names = entries.map(entry => entry?.[2]).sort();
    if (JSON.stringify(names) !== JSON.stringify([...expected].sort())) {
        throw new Error(`${manifest} must identify exactly the expected worker archives`);
    }
    for (const [, digest, name] of entries) {
        const file = join(directory, name);
        await regularFile(file);
        const hash = createHash('sha256');
        for await (const chunk of createReadStream(file)) hash.update(chunk);
        if (hash.digest('hex') !== digest) throw new Error(`Checksum mismatch: ${name}`);
    }
    return lines;
}

/** Require both variants and one shared manifest, with no probes/test payloads. */
export async function validateWorkerAssets(directory, tag) {
    const archives = workerArchives(tag);
    const names = [...archives, 'SHA256SUMS'];
    if (JSON.stringify((await readdir(directory)).sort()) !== JSON.stringify([...names].sort())) {
        throw new Error('Expected exactly glibc and musl archives plus SHA256SUMS');
    }
    await verifyManifest(directory, 'SHA256SUMS', archives);
    return names;
}

export async function assembleWorkerAssets(glibc, musl, output, tag) {
    const [glibcName, muslName] = workerArchives(tag);
    const glibcLines = await verifyManifest(glibc, 'SHA256SUMS', [glibcName]);
    const muslLines = await verifyManifest(musl, `${muslName}.sha256`, [muslName]);
    // Refuse an existing output directory: stale artifacts must never survive
    // assembly. Inputs stay untouched for target tests and release recovery.
    await mkdir(output);
    await copyFile(join(glibc, glibcName), join(output, glibcName));
    await copyFile(join(musl, muslName), join(output, muslName));
    await writeFile(join(output, 'SHA256SUMS'), [...glibcLines, ...muslLines].join('\n') + '\n');
    await validateWorkerAssets(output, tag);
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
    const args = process.argv.slice(2);
    if (args.length !== 3) throw new Error('Usage: worker-release-assets.mjs GLIBC_DIR MUSL_DIR OUTPUT_DIR');
    await assembleWorkerAssets(...args.map(path => resolve(path)), process.env.GITHUB_REF_NAME);
}
