import assert from 'node:assert/strict';
import test from 'node:test';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { loadConfig, startupConfigDocument, defaultConfig, validateConfig } from '../src/config.ts';
import { normalizeBackground } from '../shared/background.ts';
import { backgroundContentType, loadBackground, BACKGROUND_MAX_BYTES } from '../src/panel/background.ts';
import { backgroundPng, backgroundJpeg } from './helpers/background-image.ts';
import { startTestHub } from './helpers/hub.js';
import { createRest } from '../web/src/lib/rest.ts';
import { contentSecurityPolicy } from '../src/http/panel-headers.ts';

function temp(t) { const root = mkdtempSync(join(tmpdir(), 'simplex-background-')); t.after(() => rmSync(root, { recursive: true, force: true })); return root; }

test('background configuration validates union values, CSS grammar and disabled omission', t => {
    const root = temp(t);
    for (const size of ['cover', 'contain', 'auto', '960px auto', '80% auto', '.5px', 'auto 10%']) {
        assert.equal(normalizeBackground({ size, opacity: 0 }).size, size);
    }
    for (const position of ['center', 'top', 'bottom', 'left', 'right', 'top left', 'top right', 'bottom left', 'bottom right']) {
        assert.equal(normalizeBackground({ position }).position, position);
    }
    for (const raw of [null, [], { surprise: 1 }, { image: 1 }, { image: 'https://example/image' },
        { image: 'data:image/png;base64,x' }, { image: '${SECRET}' }, { size: 'url(x)' }, { size: '0px' },
        { size: '1px; color:red' }, { size: 'calc(1px)' }, { position: '2px' }, { opacity: -1 },
        { opacity: 1.1 }, { opacity: NaN }, { opacity: Infinity }, { opacity: { light: 1 } },
        { opacity: { light: 0, dark: 1, ignored: true } },
        { toolCardOpacity: -0.1 }, { toolCardOpacity: 1.1 }, { toolCardOpacity: NaN },
        { toolCardOpacity: '0.5' }, { toolCardOpacity: { light: 1 } },
        { toolCardOpacity: { light: 0, dark: 1, ignored: true } }]) {
        assert.throws(() => normalizeBackground(raw), /panel.background/);
    }
    assert.deepEqual(normalizeBackground({}).toolCardOpacity, { light: 0.6, dark: 0.6 });
    assert.deepEqual(normalizeBackground({ toolCardOpacity: 0.25 }).toolCardOpacity,
        { light: 0.25, dark: 0.25 });
    assert.deepEqual(normalizeBackground({ toolCardOpacity: { light: 0.2, dark: 0.8 } }).toolCardOpacity,
        { light: 0.2, dark: 0.8 });
    const config = loadConfig({ overrides: { dataDir: root, panel: { background: { image: '', opacity: 0.5 } } } }).config;
    assert.equal(startupConfigDocument(config).panel.background, undefined);
    assert.equal(config.panel.background, undefined);
    const unknown = defaultConfig(); unknown.panel.unknown = true;
    assert.throws(() => validateConfig(unknown), /unknown configuration/);
});

test('relative image paths and numeric opacity survive compact first-run output and reload', t => {
    const root = temp(t), file = join(root, 'hub.config.jsonc');
    writeFileSync(file, JSON.stringify({ panel: { background: { image: './wall.png', opacity: 0.3,
        size: '80% auto', position: 'bottom right', toolCardOpacity: 0.25 } } }));
    const config = loadConfig({ file }).config;
    assert.equal(config.panel.background.image, join(root, 'wall.png'));
    assert.deepEqual(config.panel.background.opacity, { light: 0.3, dark: 0.3 });
    assert.deepEqual(config.panel.background.toolCardOpacity, { light: 0.25, dark: 0.25 });
    writeFileSync(file, JSON.stringify(startupConfigDocument(config)));
    assert.deepEqual(loadConfig({ file }).config, config);
});

test('only static, structurally valid image containers are accepted', () => {
    assert.equal(backgroundContentType(backgroundPng()), 'image/png');
    assert.equal(backgroundContentType(backgroundJpeg()), 'image/jpeg');
    assert.throws(() => backgroundContentType(backgroundJpeg().subarray(0, -2)));
    for (const invalid of [Buffer.from('<svg/>'), Buffer.from('GIF89a'), Buffer.from('RIFF....WEBP'),
        backgroundPng().subarray(0, -1), backgroundPng([1, 2, 3], true), Buffer.concat([backgroundPng(), Buffer.from('tail')])]) {
        assert.throws(() => backgroundContentType(invalid));
    }
    const corrupt = backgroundPng(); corrupt[40] ^= 1;
    assert.throws(() => backgroundContentType(corrupt));
});

test('file errors disable decoration without exposing paths or failing startup', async t => {
    const root = temp(t), warnings = [];
    const log = { warn: text => warnings.push(text) };
    const oversized = join(root, 'big.png'); writeFileSync(oversized, Buffer.alloc(BACKGROUND_MAX_BYTES + 1));
    const invalid = join(root, 'invalid.png'); writeFileSync(invalid, 'not a raster');
    for (const image of [join(root, 'missing'), root, oversized, invalid]) {
        assert.equal(await loadBackground({ image }, log), null);
    }
    assert.equal(warnings.length, 4);
    assert.ok(warnings.every(text => !text.includes(root)));
});

test('asset endpoint enforces auth, immutable bytes, private caching, HEAD and safe metadata', async t => {
    const root = temp(t), image = join(root, 'wall.png'), bytes = backgroundPng();
    writeFileSync(image, bytes);
    const ctx = await startTestHub({ dataDir: join(root, 'hub'), panel: { token: 'private-token', background: { image, toolCardOpacity: 0.4 } } });
    t.after(() => ctx.hub.stop());
    const meta = await (await fetch(`${ctx.base}/api/meta`)).json();
    assert.equal(meta.background.url, '/api/panel/background/image');
    assert.deepEqual(meta.background.toolCardOpacity, { light: 0.4, dark: 0.4 });
    assert.equal(JSON.stringify(meta).includes(root), false);
    assert.equal(JSON.stringify(meta).includes('private-token'), false);
    const url = `${ctx.base}${meta.background.url}`;
    assert.equal((await fetch(url)).status, 401);
    const headers = { Authorization: 'Bearer private-token' };
    const response = await fetch(`${url}?path=/etc/passwd`, { headers });
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('content-type'), 'image/png');
    assert.equal(response.headers.get('cache-control'), 'private, no-cache');
    assert.equal(response.headers.get('x-content-type-options'), 'nosniff');
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), bytes);
    const etag = response.headers.get('etag');
    writeFileSync(image, 'changed after startup');
    const again = await fetch(url, { headers });
    assert.deepEqual(Buffer.from(await again.arrayBuffer()), bytes);
    const cached = await fetch(url, { headers: { ...headers, 'If-None-Match': `W/${etag}` } });
    assert.equal(cached.status, 304); assert.equal(await cached.text(), '');
    const head = await fetch(url, { method: 'HEAD', headers });
    assert.equal(head.status, 200); assert.equal(await head.text(), '');
    assert.equal(Number(head.headers.get('content-length')), bytes.length);
    assert.equal((await fetch(`${url}/other`, { headers })).status, 404);
    assert.equal((await fetch(url, { method: 'POST', headers })).status, 404);
});

test('disabled asset is absent from metadata and answers 404; CSP allows only image blobs', async t => {
    const root = temp(t), ctx = await startTestHub({ dataDir: root });
    t.after(() => ctx.hub.stop());
    assert.equal((await (await fetch(`${ctx.base}/api/meta`)).json()).background, undefined);
    assert.equal((await fetch(`${ctx.base}/api/panel/background/image`)).status, 404);
    const policy = contentSecurityPolicy([]);
    assert.ok(policy.includes("img-src 'self' data: blob:"));
    assert.ok(!policy.includes('script-src blob:'));
});

test('browser image fetch uses bearer header and bounds decoded transfer size', async () => {
    let called;
    const rest = createRest({ token: () => 'secret', fetchImpl: async (url, init) => {
        called = { url, init }; return new Response(backgroundPng(), { headers: { 'content-type': 'image/png' } });
    } });
    const blob = await rest.background(new AbortController().signal);
    assert.equal(blob.type, 'image/png');
    assert.equal(called.init.headers.Authorization, 'Bearer secret');
    assert.ok(!called.url.includes('secret'));
    let unauthorized = false;
    const refused = createRest({ onUnauthorized: () => { unauthorized = true; }, fetchImpl: async () => new Response('', { status: 401 }) });
    await assert.rejects(refused.background(new AbortController().signal)); assert.equal(unauthorized, true);
    const huge = createRest({ fetchImpl: async () => new Response(Buffer.alloc(BACKGROUND_MAX_BYTES + 1), { headers: { 'content-type': 'image/png' } }) });
    await assert.rejects(huge.background(new AbortController().signal), /too large/);
});
