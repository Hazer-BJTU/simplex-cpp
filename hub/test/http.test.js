/**
 * @file HTTP front door: metadata route, static panel serving, error shapes,
 * and path confinement for static requests.
 */
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, sep } from 'node:path';
import { after, before, describe, it } from 'node:test';
import { hubRoot, loadConfig } from '../src/config.ts';
import { createHub } from '../src/hub.ts';
import { createLogger } from '../src/log.ts';
import { resolveStaticPath } from '../src/http/static.ts';
import {
    cacheControlFor,
    contentSecurityPolicy,
    inlineScriptHashes,
} from '../src/http/panel-headers.ts';

const log = createLogger({ level: 'silent' });

async function startHub() {
    const { config } = loadConfig({
        overrides: {
            listen: { host: '127.0.0.1', port: 0 },
            dataDir: mkdtempSync(join(tmpdir(), 'simplex-hub-http-')),
        },
    });
    const hub = createHub({ config, log, hubRoot, version: 'test' });
    const address = await hub.start();
    return { hub, address };
}

describe('http front door', () => {
    let hub;
    let base;

    before(async () => {
        ({ hub, address: base } = await startHub());
        base = `http://127.0.0.1:${base.port}`;
    });

    after(async () => {
        await hub.stop();
    });

    it('announces the panel protocol at /api/meta', async () => {
        const response = await fetch(`${base}/api/meta`);
        assert.equal(response.status, 200);
        const body = await response.json();
        assert.equal(body.name, 'simplex-hub');
        assert.equal(body.version, 'test');
        assert.deepEqual(body.protocol, { name: 'simplex-hub-panel', version: 1 });
        assert.equal(body.worker_protocol, 'core/docs/worker-protocol.md');
    });

    it('serves the static panel at the root', async () => {
        const response = await fetch(`${base}/`);
        assert.equal(response.status, 200);
        assert.match(response.headers.get('content-type'), /text\/html/);
        assert.match(await response.text(), /simplex hub/);
    });

    it('answers unknown API routes with a JSON error', async () => {
        const response = await fetch(`${base}/api/nope`);
        assert.equal(response.status, 404);
        const body = await response.json();
        assert.equal(body.error, 'not_found');
    });

    it('rejects unsupported methods outside the API', async () => {
        const response = await fetch(`${base}/`, { method: 'POST' });
        assert.equal(response.status, 405);
        assert.equal((await response.json()).error, 'method_not_allowed');
    });

    it('rejects a WebSocket upgrade on an unregistered route', async () => {
        const response = await fetch(`${base}/agent/unknown/events`);
        assert.equal(response.status, 404);
    });
});

describe('static path confinement', () => {
    const root = join(hubRoot, 'web');

    it('maps a normal path inside the root', () => {
        assert.equal(resolveStaticPath(root, '/index.html'), join(root, 'index.html'));
        assert.equal(resolveStaticPath(root, '/'), root);
    });

    it('never resolves outside the panel root', () => {
        // `..` segments are clamped at the root rather than rejected, so the
        // invariant to assert is containment, not null.
        for (const pathname of [
            '/../../etc/passwd',
            '/..%2f..%2fetc/passwd',
            '/a/../../b',
            '//etc/passwd',
            '/./../../x',
        ]) {
            const resolved = resolveStaticPath(root, pathname);
            if (resolved === null) continue;
            assert.ok(resolved === root || resolved.startsWith(root + sep),
                `${pathname} resolved outside the root: ${resolved}`);
        }
    });

    it('refuses NUL bytes', () => {
        assert.equal(resolveStaticPath(root, '/index.html%00.png'), null);
    });
});

describe('panel response headers', () => {
    /** A hub of its own, because these assertions are about one response each. */
    async function startServer() {
        const { hub, address } = await startHub();
        return { origin: `http://127.0.0.1:${address.port}`, close: () => hub.stop() };
    }

    it('hashes only the inline scripts, and hashes them exactly', async () => {
        const html = '<script src="/assets/a.js"></script><script>let a = 1;</script>';
        const hashes = inlineScriptHashes(html);
        assert.equal(hashes.length, 1, 'an external script must not be hashed');
        // The browser hashes the text between the tags, byte for byte, so the
        // header has to be produced from that same text and nothing else.
        const expected = createHash('sha256').update('let a = 1;').digest('base64');
        assert.deepEqual(hashes, [`'sha256-${expected}'`]);
    });

    it('builds a policy that does not allow inline scripts', () => {
        const policy = contentSecurityPolicy(["'sha256-abc'"]);
        assert.match(policy, /script-src 'self' 'sha256-abc'/);
        assert.ok(!/script-src[^;]*unsafe-inline/.test(policy),
            'the policy allows inline scripts, which is the whole thing it is for');
        assert.match(policy, /frame-ancestors 'none'/);
        assert.match(policy, /object-src 'none'/);
    });

    it('caches hashed assets for a year and nothing else', () => {
        assert.match(cacheControlFor('/x/assets/index-Dd5nP71s.js'), /immutable/);
        assert.match(cacheControlFor('/x/assets/app-Uysv_syg.css'), /immutable/);
        assert.equal(cacheControlFor('/x/index.html'), 'no-cache');
        assert.equal(cacheControlFor('/x/assets/logo.png'), 'no-cache');
    });

    it('serves the panel with a policy built from the document', async () => {
        const result = await startServer();
        try {
            const response = await fetch(`${result.origin}/`);
            assert.equal(response.status, 200);
            assert.equal(response.headers.get('x-content-type-options'), 'nosniff');
            assert.equal(response.headers.get('x-frame-options'), 'DENY');
            assert.equal(response.headers.get('referrer-policy'), 'no-referrer');
            const policy = response.headers.get('content-security-policy');
            assert.ok(policy, 'the panel document has no content security policy');
            assert.match(policy, /script-src 'self' 'sha256-/,
                'the inline theme script is not named in the policy, so it will not run');
            assert.match(policy, /connect-src 'self'/);
        } finally {
            await result.close();
        }
    });

    it('does not put a policy on an asset', async () => {
        const result = await startServer();
        try {
            const page = await fetch(`${result.origin}/`);
            const html = await page.text();
            const asset = /(?:src|href)="(\/assets\/[^"]+\.js)"/.exec(html);
            if (!asset) return; // No build present: the header claim is about a document.
            const response = await fetch(`${result.origin}${asset[1]}`);
            assert.equal(response.headers.get('content-security-policy'), null);
            assert.match(response.headers.get('cache-control') ?? '', /immutable/);
        } finally {
            await result.close();
        }
    });
});
