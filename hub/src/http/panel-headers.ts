/**
 * @file the response headers the panel is served with.
 *
 * The panel is the hub's front door on loopback, and it renders text it did not
 * write — model output, tool output, log lines. Two of these headers are about
 * that: `nosniff` stops a browser from re-interpreting a file as a type the hub
 * did not declare, and the content security policy stops the page from being
 * talked into loading anything that is not part of it.
 *
 * The policy is built from the document rather than hardcoded, because the
 * panel has exactly one inline script (the theme, applied before the bundle so
 * a dark preference does not flash white) and a policy that named it by hash
 * would have to be edited every time that script changed. The hashes are
 * computed from the built file, so the header and the document cannot disagree.
 *
 * `style-src` allows inline styles, and that is not laziness: Radix positions
 * its poppers with a `style` attribute React writes, so a policy without
 * `'unsafe-inline'` there would move every menu and dialog to the top-left
 * corner. `script-src` does not, and that is the one that matters.
 */
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';

/** Headers that are the same for every panel response. */
export const SECURITY_HEADERS: Record<string, string> = {
    'X-Content-Type-Options': 'nosniff',
    'Referrer-Policy': 'no-referrer',
    'X-Frame-Options': 'DENY',
    'Cross-Origin-Opener-Policy': 'same-origin',
};

/**
 * The `sha256-…` sources for a document's inline scripts.
 *
 * A script with a `src` is covered by `'self'` and is deliberately not hashed:
 * hashing it would pin the bundle's filename into the header for no gain.
 */
export function inlineScriptHashes(html: string): string[] {
    const hashes: string[] = [];
    const pattern = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
    for (const match of html.matchAll(pattern)) {
        const body = match[1] ?? '';
        hashes.push(`'sha256-${createHash('sha256').update(body).digest('base64')}'`);
    }
    return hashes;
}

/** The policy for a document with the given inline scripts. */
export function contentSecurityPolicy(scriptHashes: readonly string[]): string {
    return [
        "default-src 'self'",
        ['script-src', "'self'", ...scriptHashes].join(' '),
        "style-src 'self' 'unsafe-inline'",
        "img-src 'self' data:",
        "font-src 'self'",
        // The panel socket is the same origin, which `'self'` covers for `ws:`
        // as well as `http:`.
        "connect-src 'self'",
        "object-src 'none'",
        "base-uri 'none'",
        "form-action 'none'",
        "frame-ancestors 'none'",
    ].join('; ');
}

/**
 * The headers for one panel document, hashes included.
 *
 * Called per response rather than cached at startup so that a rebuild is picked
 * up without a restart — the hub serves `web/dist` from disk, and a developer
 * who runs `npm run build` should not have to restart the hub to see it.
 */
export async function panelHeaders(root: string, path: string): Promise<Record<string, string>> {
    const headers: Record<string, string> = { ...SECURITY_HEADERS };
    if (!path.endsWith('.html')) return headers;
    try {
        const html = await readFile(path, 'utf8');
        headers['Content-Security-Policy'] = contentSecurityPolicy(inlineScriptHashes(html));
    } catch {
        // Unreadable means the file is about to 404 or 500 on its own; a policy
        // invented here would only confuse that.
    }
    return headers;
}

/**
 * How long a file may be cached.
 *
 * Vite writes `index-<hash>.js` and `index-<hash>.css`, whose names change with
 * their contents, so those are safe to keep for a year. Everything else — the
 * document above all — must be revalidated, because it is what names the hashed
 * files.
 */
export function cacheControlFor(path: string): string {
    const hashed = /-[A-Za-z0-9_-]{8,}\.(?:js|css|woff2?|png|svg|ico)$/.test(path);
    return hashed ? 'public, max-age=31536000, immutable' : 'no-cache';
}
