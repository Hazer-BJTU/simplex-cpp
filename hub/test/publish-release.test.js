import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { chmodSync, existsSync, mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, test } from 'node:test';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { RegistryProbeError, registryIntegrity, verifyPublished, verifyPublishedWithRetry } from '../scripts/publish-release.mjs';

const directory = mkdtempSync(join(tmpdir(), 'simplex-npm-test-'));
const archive = join(directory, 'package.tgz');
writeFileSync(archive, Buffer.from('the exact packed bytes'));
const integrity = `sha512-${createHash('sha512').update('the exact packed bytes').digest('base64')}`;
after(() => rmSync(directory, { recursive: true, force: true }));

test('release retry accepts only byte-identical npm publications', async t => {
    let present = false;
    let remoteIntegrity = integrity;
    const server = createServer((request, response) => {
        assert.equal(request.url, '/%40hazer-bjtu%2Fsimplex-hub/0.1.0');
        if (!present) {
            response.writeHead(404).end();
            return;
        }
        response.setHeader('Content-Type', 'application/json');
        response.end(JSON.stringify({
            name: '@hazer-bjtu/simplex-hub',
            version: '0.1.0',
            dist: { integrity: remoteIntegrity },
        }));
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    t.after(() => new Promise(resolve => server.close(resolve)));
    const registry = `http://127.0.0.1:${server.address().port}`;
    const name = '@hazer-bjtu/simplex-hub';
    assert.equal(await registryIntegrity(name, '0.1.0', registry), null);
    assert.equal(await verifyPublished(archive, name, '0.1.0', registry), false);
    present = true;
    assert.equal(await verifyPublished(archive, name, '0.1.0', registry), true);
    remoteIntegrity = 'sha512-other';
    await assert.rejects(verifyPublished(archive, name, '0.1.0', registry), /differs/);
});

test('verification retries absent versions, transient HTTP/network errors and unreadable responses', async () => {
    const responses = [
        new Response(null, { status: 404 }),
        new TypeError('connection reset'),
        new Response(null, { status: 503 }),
        new Response(null, { status: 429 }),
        new Response('{unfinished'),
        Response.json({ name: 'fixture', version: '1.0.0', dist: { integrity } }),
    ];
    let calls = 0;
    let waits = 0;
    const matched = await verifyPublishedWithRetry(archive, 'fixture', '1.0.0', 'https://fixture', {
        attempts: responses.length,
        fetcher: async (_url, { signal }) => {
            assert.ok(signal instanceof AbortSignal);
            const response = responses[calls++];
            if (response instanceof Error) throw response;
            return response;
        },
        sleep: async milliseconds => { assert.equal(milliseconds, 3000); waits++; },
    });
    assert.equal(matched, true);
    assert.equal(calls, responses.length);
    assert.equal(waits, calls - 1);
});

test('verification bounds retries and does not mistake registry outages for an absent package', async () => {
    let calls = 0;
    let waits = 0;
    await assert.rejects(verifyPublishedWithRetry(archive, 'fixture', '1.0.0', 'https://fixture', {
        attempts: 3,
        waitForPublication: false,
        fetcher: async () => { calls++; return new Response(null, { status: 503 }); },
        sleep: async () => { waits++; },
    }), RegistryProbeError);
    assert.equal(calls, 3);
    assert.equal(waits, 2);

    calls = 0;
    assert.equal(await verifyPublishedWithRetry(archive, 'fixture', '1.0.0', 'https://fixture', {
        waitForPublication: false,
        fetcher: async () => { calls++; return new Response(null, { status: 404 }); },
        sleep: async () => assert.fail('preflight must not poll an absent package'),
    }), false);
    assert.equal(calls, 1);

    calls = 0;
    assert.equal(await verifyPublishedWithRetry(archive, 'fixture', '1.0.0', 'https://fixture', {
        attempts: 3,
        fetcher: async () => { calls++; return new Response(null, { status: 404 }); },
        sleep: async () => {},
    }), false);
    assert.equal(calls, 3);
});

test('integrity, identity and permanent HTTP failures abort verification without retry', async () => {
    for (const response of [
        new Response(null, { status: 401 }),
        new Response(null, { status: 403 }),
        Response.json({ name: 'fixture', version: '1.0.0', dist: { integrity: 'sha512-other' } }),
        Response.json({ name: 'wrong-package', version: '1.0.0', dist: { integrity } }),
        Response.json({ name: 'fixture', version: 'wrong-version', dist: { integrity } }),
        Response.json({ name: 'fixture', version: '1.0.0' }),
    ]) {
        let calls = 0;
        await assert.rejects(verifyPublishedWithRetry(archive, 'fixture', '1.0.0', 'https://fixture', {
            fetcher: async () => { calls++; return response; },
            sleep: async () => assert.fail('fatal probe failure must not be retried'),
        }), error => !(error instanceof RegistryProbeError));
        assert.equal(calls, 1);
    }
});

test('registry timeout bounds both response headers and stalled JSON bodies', { timeout: 5000 }, async t => {
    const server = createServer((request, response) => {
        if (request.url.includes('body')) {
            response.writeHead(200, { 'Content-Type': 'application/json' });
            response.write('{');
        }
        // Leave either headers or the body pending until the probe aborts.
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    t.after(() => {
        server.closeAllConnections();
        return new Promise(resolve => server.close(resolve));
    });
    for (const name of ['headers', 'body']) {
        await assert.rejects(registryIntegrity(name, '1.0.0', `http://127.0.0.1:${server.address().port}`, {
            timeoutMs: 50,
        }), RegistryProbeError);
    }
});

/** Exercise the actual CLI while npm and registry mutations remain local fixtures. */
async function runPublisher(t, { transientResponses = [], publishExitCode = 0 } = {}) {
    const work = mkdtempSync(join(tmpdir(), 'simplex-npm-publish-'));
    t.after(() => rmSync(work, { recursive: true, force: true }));
    const bin = join(work, 'bin');
    mkdirSync(bin);
    const tarball = join(work, 'package.tgz');
    writeFileSync(tarball, 'packed bytes');
    const flag = join(work, 'published');
    const npm = join(bin, 'npm');
    writeFileSync(npm, `#!/usr/bin/env node
const fs = require('node:fs');
const path = require('node:path');
if (process.argv[2] !== 'publish' || !path.isAbsolute(process.argv[3])) process.exit(42);
const marker = process.env.FAKE_PUBLISHED;
const calls = fs.existsSync(marker) ? Number(fs.readFileSync(marker, 'utf8')) + 1 : 1;
fs.writeFileSync(marker, String(calls));
process.exit(Number(process.env.FAKE_PUBLISH_EXIT_CODE));
`);
    chmodSync(npm, 0o755);
    const manifest = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));
    const expected = `sha512-${createHash('sha512').update('packed bytes').digest('base64')}`;
    let probesAfterPublish = 0;
    const server = createServer((request, response) => {
        assert.equal(request.url, `/${encodeURIComponent(manifest.name)}/${manifest.version}`);
        if (!existsSync(flag)) {
            response.writeHead(404).end();
            return;
        }
        const temporary = transientResponses[probesAfterPublish++];
        if (temporary) {
            response.writeHead(temporary).end();
            return;
        }
        response.setHeader('Content-Type', 'application/json');
        response.end(JSON.stringify({
            name: manifest.name,
            version: manifest.version,
            dist: { integrity: expected },
        }));
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    t.after(() => new Promise(resolve => server.close(resolve)));
    const script = fileURLToPath(new URL('../scripts/publish-release.mjs', import.meta.url));
    const result = await new Promise(resolve => {
        const child = spawn(process.execPath, [script, 'package.tgz'], {
            cwd: work,
            env: {
                ...process.env,
                NPM_CONFIG_REGISTRY: `http://127.0.0.1:${server.address().port}`,
                FAKE_PUBLISHED: flag,
                FAKE_PUBLISH_EXIT_CODE: String(publishExitCode),
                PATH: `${bin}:${process.env.PATH}`,
            },
        });
        let error = '';
        child.stderr.on('data', data => { error += data; });
        child.on('close', code => resolve({ code, error }));
    });
    return { ...result, published: existsSync(flag), probesAfterPublish,
        publishCalls: existsSync(flag) ? Number(readFileSync(flag, 'utf8')) : 0 };
}

test('publishes a relative archive as an absolute file path', async t => {
    const result = await runPublisher(t);
    assert.equal(result.code, 0, result.error);
    assert.equal(result.published, true);
    assert.equal(result.publishCalls, 1);
    assert.equal(result.probesAfterPublish, 1);
});

test('successful publication survives a temporary registry failure during the actual CLI verification', async t => {
    const result = await runPublisher(t, { transientResponses: [503] });
    assert.equal(result.code, 0, result.error);
    assert.equal(result.published, true);
    assert.equal(result.publishCalls, 1);
    assert.equal(result.probesAfterPublish, 2);
});

test('a failed publish client response is accepted only after matching registry integrity', async t => {
    const result = await runPublisher(t, { publishExitCode: 1 });
    assert.equal(result.code, 0, result.error);
    assert.equal(result.published, true);
    assert.equal(result.publishCalls, 1);
});
