/** Installer-scoped network transport; never changes the Hub's global dispatcher. */
import { EnvHttpProxyAgent, fetch } from 'undici';

// Keep this seam limited to the fields the downloader uses. The npm Undici
// fetch and its dispatcher belong to the same version; Node's separately
// bundled dispatcher/Request types can differ across supported Node releases.
export type DownloadResponse = Pick<Response, 'ok' | 'status' | 'body' | 'text'>;
export type Fetch = (url: string, options?: {
    signal?: AbortSignal | null;
    headers?: Record<string, string>;
}) => Promise<DownloadResponse>;

/**
 * Read HTTP_PROXY/HTTPS_PROXY/NO_PROXY (including their lowercase variants)
 * for each download operation. The same dispatcher follows asset redirects
 * and stays alive until the streamed response has been consumed. Destroy its
 * connections on every exit, including cancellation and partial-body errors.
 * Explicitly supplied fetchers retain ownership of their transport.
 */
export async function withDownloadFetch<T>(fetcher: Fetch | undefined,
    operation: (fetcher: Fetch) => Promise<T>): Promise<T> {
    if (fetcher) return operation(fetcher);
    const dispatcher = new EnvHttpProxyAgent();
    const download: Fetch = (url, options) => fetch(url, { ...options, dispatcher });
    try {
        return await operation(download);
    } finally {
        await dispatcher.destroy();
    }
}
