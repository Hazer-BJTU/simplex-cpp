/** Resolve official GitHub release assets without coupling to installation. */
import { normalizeVersion } from './version.ts';

const REPOSITORY = 'Hazer-BJTU/simplex-cpp';
export interface WorkerRelease {
    source: 'github';
    version: string;
    root: string;
    archive: string;
    archiveUrl: string;
    checksumUrl: string;
}
export type Fetch = typeof globalThis.fetch;

/** Bound both headers and streamed response body by the caller's operation timeout. */
export async function fetchResponse(url: string, fetcher: Fetch, signal: AbortSignal): Promise<Response> {
    let response: Response;
    try {
        response = await fetcher(url, {
            signal,
            headers: { 'User-Agent': 'simplex-hub-worker-installer', Accept: 'application/vnd.github+json' },
        });
    } catch (error) {
        const failure = error as Error & { cause?: Error };
        throw new Error(`Could not fetch ${url}: ${failure.cause?.message ?? failure.message}`);
    }
    if (!response.ok) {
        await response.body?.cancel();
        throw new Error(`Download failed (HTTP ${response.status}): ${url}`);
    }
    return response;
}

/** Small JSON/checksum responses have a separate memory limit. */
export async function readBounded(response: Response, limit: number): Promise<Buffer> {
    const chunks: Uint8Array[] = [];
    let size = 0;
    if (!response.body) throw new Error('Release response has no body');
    for await (const chunk of response.body) {
        size += chunk.length;
        if (size > limit) throw new Error('Release response exceeds the size limit');
        chunks.push(chunk);
    }
    return Buffer.concat(chunks);
}

/** Only published stable releases with exactly matching worker/checksum assets qualify. */
export async function resolveRelease(source: string, requested?: string,
    fetcher: Fetch = fetch, signal?: AbortSignal): Promise<WorkerRelease> {
    if (source !== 'github') throw new Error(`Unsupported source: ${source}; only github is supported`);
    const selector = requested ? `tags/v${normalizeVersion(requested)}` : 'latest';
    const url = `https://api.github.com/repos/${REPOSITORY}/releases/${selector}`;
    const timeout = AbortSignal.timeout(30_000);
    const response = await fetchResponse(url, fetcher, signal ? AbortSignal.any([signal, timeout]) : timeout);
    const value = JSON.parse((await readBounded(response, 2 * 1024 * 1024)).toString('utf8')) as {
        draft: boolean; prerelease: boolean; tag_name: string;
        assets: { name: string; browser_download_url: string }[];
    };
    if (!value || value.draft !== false || value.prerelease !== false || typeof value.tag_name !== 'string'
        || !Array.isArray(value.assets) || value.assets.some(asset => !asset || typeof asset.name !== 'string')) {
        throw new Error('Expected a published stable GitHub release');
    }
    const version = normalizeVersion(value.tag_name);
    if (value.tag_name !== `v${version}` || (requested && version !== normalizeVersion(requested))) {
        throw new Error('Release tag does not match the requested stable version');
    }
    const root = `simplex-worker-v${version}-linux-x86_64-glibc2.34`;
    const archive = `${root}.tar.gz`;
    const assetUrl = (name: string): string => {
        const matches = value.assets.filter(asset => asset.name === name);
        if (matches.length !== 1) throw new Error(`Release v${version} must contain one ${name} asset`);
        const url = matches[0]!.browser_download_url;
        const expected = `https://github.com/${REPOSITORY}/releases/download/v${version}/${name}`;
        if (url !== expected) throw new Error(`Unexpected official release asset URL for ${name}`);
        return url;
    };
    return { source: 'github', version, root, archive,
        archiveUrl: assetUrl(archive), checksumUrl: assetUrl('SHA256SUMS') };
}
