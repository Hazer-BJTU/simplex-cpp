/**
 * @file static file serving for the hub's web panel.
 *
 * The panel is dependency-free HTML/CSS/JS served straight from `hub/web`, so
 * there is no build step and no bundler in the deployment path. Only regular
 * files below the panel root are reachable; every resolution is confined to
 * that root.
 */
import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import { extname, join, normalize, resolve, sep } from 'node:path';
import type { ServerResponse } from 'node:http';

const CONTENT_TYPES: Record<string, string> = {
    '.html': 'text/html; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.svg': 'image/svg+xml',
    '.png': 'image/png',
    '.ico': 'image/x-icon',
    '.woff2': 'font/woff2',
    '.txt': 'text/plain; charset=utf-8',
    '.map': 'application/json; charset=utf-8',
};

/** Content type for a path, defaulting to a byte stream. */
export function contentTypeFor(path: string): string {
    return CONTENT_TYPES[extname(path).toLowerCase()] ?? 'application/octet-stream';
}

/**
 * Map a URL pathname onto a file below `root`, or return null when the request
 * escapes the root or is not a servable path.
 */
export function resolveStaticPath(root: string, pathname: string): string | null {
    let decoded: string;
    try {
        decoded = decodeURIComponent(pathname);
    } catch {
        // A malformed escape is a bad request; the caller answers 400 for null.
        return null;
    }
    if (decoded.includes('\0')) return null;
    const relative = normalize(decoded).replace(/^([/\\])+/, '');
    const target = resolve(join(root, relative));
    if (target !== root && !target.startsWith(root + sep)) return null;
    return target;
}

/** Everything `serveStatic` needs. */
export interface ServeStaticOptions {
    /** Absolute panel directory. */
    root: string;
    /** Request pathname, still percent-encoded. */
    pathname: string;
    res: ServerResponse;
    /** Debug sink; a miss is not an error. */
    log?: (level: string, message: string) => void;
}

/**
 * Serve one request from `root`.
 *
 * @returns true when a response was written. Every path in this version writes
 *   one, so the result is currently always true — it is kept because the SPA
 *   fallback the panel rewrite needs will return false for an unknown path and
 *   let the caller answer instead.
 */
export async function serveStatic({ root, pathname, res, log }: ServeStaticOptions): Promise<boolean> {
    let target = resolveStaticPath(root, pathname);
    if (!target) {
        res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('bad path\n');
        return true;
    }
    let info = await stat(target).catch(() => null);
    if (info?.isDirectory()) {
        target = join(target, 'index.html');
        info = await stat(target).catch(() => null);
    }
    if (!info?.isFile()) {
        log?.('debug', `static miss ${pathname}`);
        res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('not found\n');
        return true;
    }
    res.writeHead(200, {
        'Content-Type': contentTypeFor(target),
        'Content-Length': info.size,
        'Cache-Control': 'no-cache',
    });
    if (res.req?.method === 'HEAD') {
        res.end();
        return true;
    }
    await new Promise<void>((done) => {
        const stream = createReadStream(target);
        stream.on('error', () => {
            res.destroy();
            done();
        });
        stream.on('end', done);
        stream.pipe(res);
    });
    return true;
}
