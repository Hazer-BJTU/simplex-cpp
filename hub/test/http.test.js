/**
 * @file HTTP front door: metadata route, static panel serving, error shapes,
 * and path confinement for static requests.
 */
import assert from 'node:assert/strict';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, sep } from 'node:path';
import { after, before, describe, it } from 'node:test';
import { hubRoot, loadConfig } from '../src/config.js';
import { createHub } from '../src/hub.js';
import { createLogger } from '../src/log.ts';
import { resolveStaticPath } from '../src/http/static.js';

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
