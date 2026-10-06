#!/usr/bin/env node
/** Publish the exact packed tarball, or verify an already-published copy. */
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { execFileSync } from 'node:child_process';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

/** A probe may be retried; publishing itself must never be retried here. */
export class RegistryProbeError extends Error {}

export async function registryIntegrity(name, version, registry = 'https://registry.npmjs.org', {
    fetcher = fetch,
    timeoutMs = 10_000,
} = {}) {
    const url = `${registry.replace(/\/$/, '')}/${encodeURIComponent(name)}/${encodeURIComponent(version)}`;
    let response;
    try {
        response = await fetcher(url, { signal: AbortSignal.timeout(timeoutMs) });
    } catch (error) {
        throw new RegistryProbeError(`npm registry request failed for ${name}@${version}`, { cause: error });
    }
    if (!response.ok) {
        // No error body is needed. Release the connection before another probe;
        // a failure to cancel an already-aborted body cannot change its status.
        try { await response.body?.cancel(); } catch {}
    }
    if (response.status === 404) return null;
    if ([408, 425, 429].includes(response.status) || response.status >= 500 && response.status <= 599) {
        throw new RegistryProbeError(`npm registry returned HTTP ${response.status} for ${name}@${version}`);
    }
    if (!response.ok) throw new Error(`npm registry returned HTTP ${response.status} for ${name}@${version}`);
    let metadata;
    try {
        metadata = await response.json();
    } catch (error) {
        // The response body can time out or be cut short after headers arrive.
        throw new RegistryProbeError(`npm registry response could not be read for ${name}@${version}`, { cause: error });
    }
    if (metadata?.name !== name || metadata.version !== version || !metadata.dist?.integrity) {
        throw new Error(`npm registry returned incomplete metadata for ${name}@${version}`);
    }
    return metadata.dist.integrity;
}

async function expectedIntegrity(archive) {
    return `sha512-${createHash('sha512').update(await readFile(archive)).digest('base64')}`;
}

async function matchesPublished(expected, name, version, registry, probe) {
    const published = await registryIntegrity(name, version, registry, probe);
    if (published && published !== expected) {
        throw new Error(`Existing npm package ${name}@${version} differs from the tested tarball`);
    }
    return Boolean(published);
}

export async function verifyPublished(archive, name, version, registry) {
    return matchesPublished(await expectedIntegrity(archive), name, version, registry);
}

/**
 * Only absent publications and transient probes can be retried. Authentication,
 * mismatched/incomplete metadata and integrity mismatches remain fatal. A preflight probe
 * returns immediately on 404, but an unavailable registry never authorizes a
 * publish. Injected transport/delay keep the retry and exhaustion tests bounded.
 */
export async function verifyPublishedWithRetry(archive, name, version, registry, {
    attempts = 10,
    delayMs = 3000,
    waitForPublication = true,
    fetcher = fetch,
    timeoutMs = 10_000,
    sleep = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds)),
} = {}) {
    const expected = await expectedIntegrity(archive);
    let lastError;
    for (let attempt = 0; attempt < attempts; attempt++) {
        try {
            if (await matchesPublished(expected, name, version, registry, { fetcher, timeoutMs })) return true;
            lastError = undefined;
            if (!waitForPublication) return false;
        } catch (error) {
            if (!(error instanceof RegistryProbeError)) throw error;
            lastError = error;
        }
        if (attempt < attempts - 1) await sleep(delayMs);
    }
    if (lastError) throw lastError;
    return false;
}

async function main() {
    const archive = process.argv[2] && resolve(process.argv[2]);
    if (!archive || process.argv.length > 4 ||
        (process.argv[3] && process.argv[3] !== '--verify-only')) {
        throw new Error('Usage: node scripts/publish-release.mjs PACKAGE.tgz [--verify-only]');
    }
    const manifest = JSON.parse(await readFile(new URL('../package.json', import.meta.url), 'utf8'));
    const registry = process.env.NPM_CONFIG_REGISTRY || 'https://registry.npmjs.org';
    const published = await verifyPublishedWithRetry(archive, manifest.name, manifest.version, registry, {
        attempts: 3,
        waitForPublication: false,
    });
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
    try {
        if (await verifyPublishedWithRetry(archive, manifest.name, manifest.version, registry)) return;
    } catch (error) {
        if (publishError) throw new AggregateError([publishError, error], 'npm publish and registry verification failed');
        throw error;
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
