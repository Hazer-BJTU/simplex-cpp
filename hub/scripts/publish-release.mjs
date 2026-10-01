#!/usr/bin/env node
/** Publish the exact packed tarball, or verify an already-published copy. */
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

export async function registryIntegrity(name, version, registry = 'https://registry.npmjs.org') {
    const url = `${registry.replace(/\/$/, '')}/${encodeURIComponent(name)}/${encodeURIComponent(version)}`;
    const response = await fetch(url);
    if (response.status === 404) return null;
    if (!response.ok) throw new Error(`npm registry returned HTTP ${response.status} for ${name}@${version}`);
    const metadata = await response.json();
    if (metadata.name !== name || metadata.version !== version || !metadata.dist?.integrity) {
        throw new Error(`npm registry returned incomplete metadata for ${name}@${version}`);
    }
    return metadata.dist.integrity;
}

export async function verifyPublished(archive, name, version, registry) {
    const expected = `sha512-${createHash('sha512').update(await readFile(archive)).digest('base64')}`;
    const published = await registryIntegrity(name, version, registry);
    if (published && published !== expected) {
        throw new Error(`Existing npm package ${name}@${version} differs from the tested tarball`);
    }
    return Boolean(published);
}

async function main() {
    const archive = process.argv[2] && resolve(process.argv[2]);
    if (!archive || process.argv.length > 4 ||
        (process.argv[3] && process.argv[3] !== '--verify-only')) {
        throw new Error('Usage: node scripts/publish-release.mjs PACKAGE.tgz [--verify-only]');
    }
    const manifest = JSON.parse(await readFile(new URL('../package.json', import.meta.url), 'utf8'));
    const registry = process.env.NPM_CONFIG_REGISTRY || 'https://registry.npmjs.org';
    const published = await verifyPublished(archive, manifest.name, manifest.version, registry);
    if (published) {
        console.log(`Verified existing npm package ${manifest.name}@${manifest.version}`);
        return;
    }
    if (process.argv[3] === '--verify-only') {
        console.log(`npm package ${manifest.name}@${manifest.version} is not published`);
        return;
    }
    let publishError;
    try {
        execFileSync('npm', ['publish', archive, '--access', 'public'], { stdio: 'inherit' });
    } catch (error) {
        publishError = error;
    }
    // Publication may have succeeded even if the client lost the response.
    for (let attempt = 0; attempt < 10; attempt++) {
        if (await verifyPublished(archive, manifest.name, manifest.version, registry)) return;
        if (attempt < 9) await new Promise(resolve => setTimeout(resolve, 3000));
    }
    if (publishError) throw publishError;
    throw new Error(`npm did not expose the published ${manifest.name}@${manifest.version} tarball`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
    main().catch(error => {
        console.error(error);
        process.exitCode = 1;
    });
}
