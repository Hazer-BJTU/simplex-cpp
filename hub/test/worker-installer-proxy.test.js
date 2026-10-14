/** Real loopback proxies exercise the default downloader, including TLS and redirects. */
import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createServer } from 'node:http';
import { createServer as createHttpsServer } from 'node:https';
import { connect } from 'node:net';
import { execFile, execFileSync } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdtemp, mkdir, readFile, writeFile, rm, readdir } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { createHash } from 'node:crypto';
import * as tar from 'tar';
import { getGlobalDispatcher } from 'undici';
import { withDownloadFetch } from '../src/install/network.ts';

const exec = promisify(execFile);
const proxyVariables = ['HTTP_PROXY', 'HTTPS_PROXY', 'NO_PROXY', 'http_proxy', 'https_proxy', 'no_proxy',
    'ALL_PROXY', 'all_proxy', 'NODE_USE_ENV_PROXY', 'NODE_TLS_REJECT_UNAUTHORIZED'];

function environment(overrides = {}) {
    const env = { ...process.env };
    for (const name of proxyVariables) delete env[name];
    return { ...env, ...overrides };
}

async function withEnvironment(overrides, operation) {
    const previous = { ...process.env };
    for (const name of proxyVariables) delete process.env[name];
    Object.assign(process.env, overrides);
    try { return await operation(); }
    finally {
        for (const name of proxyVariables) {
            if (previous[name] === undefined) delete process.env[name]; else process.env[name] = previous[name];
        }
    }
}

/** Destroy owned sockets on teardown so an assertion cannot hang the test runner. */
async function listen(t, server) {
    const sockets = new Set();
    server.on('connection', socket => {
        sockets.add(socket);
        socket.on('error', () => {});
        socket.once('close', () => sockets.delete(socket));
    });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    t.after(async () => {
        for (const socket of sockets) socket.destroy();
        await new Promise(resolve => server.close(resolve));
    });
    return { server, sockets, port: server.address().port, url: `http://127.0.0.1:${server.address().port}` };
}

/** CONNECT works for HTTP and HTTPS; target routing is confined to loopback. */
async function proxy(t, targetPort) {
    const tunnels = [];
    const server = createServer((_req, response) => response.writeHead(502).end());
    const endpoint = await listen(t, server);
    server.on('connect', (request, socket, head) => {
        tunnels.push(request.url);
        if (!targetPort) {
            socket.end('HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n');
            return;
        }
        const upstream = connect(targetPort, '127.0.0.1', () => {
            socket.write('HTTP/1.1 200 Connection Established\r\n\r\n');
            if (head.length) upstream.write(head);
            socket.pipe(upstream).pipe(socket);
        });
        upstream.on('error', () => socket.destroy());
        socket.once('close', () => upstream.destroy());
        upstream.once('close', () => socket.destroy());
    });
    return { ...endpoint, tunnels };
}

async function get(url) {
    return withDownloadFetch(undefined, async fetcher => {
        const response = await fetcher(url, { signal: AbortSignal.timeout(2000) });
        return response.text();
    });
}

test('HTTP proxy variables select the installer dispatcher, including lowercase precedence', async t => {
    const origin = await listen(t, createServer((_request, response) => response.end('proxied')));
    const upper = await proxy(t, origin.port);
    const lower = await proxy(t, origin.port);
    const global = getGlobalDispatcher();
    await withEnvironment({ HTTP_PROXY: upper.url }, async () => {
        assert.equal(await get('http://download.invalid/asset'), 'proxied');
    });
    await withEnvironment({ HTTP_PROXY: upper.url, http_proxy: lower.url }, async () => {
        assert.equal(await get('http://download.invalid/asset'), 'proxied');
    });
    assert.deepEqual(upper.tunnels, ['download.invalid:80']);
    assert.deepEqual(lower.tunnels, ['download.invalid:80']);
    assert.equal(getGlobalDispatcher(), global);
});

test('no proxy environment uses a direct connection; NO_PROXY honors hosts, ports and lowercase', async t => {
    const origin = await listen(t, createServer((_request, response) => response.end('direct')));
    const rejected = await proxy(t, null);
    for (const bypass of ['127.0.0.1', `127.0.0.1:${origin.port}`, '*']) {
        await withEnvironment({ HTTP_PROXY: rejected.url, NO_PROXY: bypass }, async () => {
            assert.equal(await get(origin.url), 'direct');
        });
    }
    await withEnvironment({}, async () => assert.equal(await get(origin.url), 'direct'));
    await withEnvironment({ HTTP_PROXY: rejected.url, NO_PROXY: 'ignore.invalid', no_proxy: '*' },
        async () => assert.equal(await get(origin.url), 'direct'));
    assert.equal(rejected.tunnels.length, 0);
    // A mismatched port must not silently bypass the configured proxy.
    await withEnvironment({ HTTP_PROXY: rejected.url, NO_PROXY: '127.0.0.1:1' },
        async () => assert.rejects(get(origin.url)));
    assert.equal(rejected.tunnels.length, 1);
});

test('redirect destinations are dispatched through the configured proxy too', async t => {
    const origin = await listen(t, createServer((request, response) => {
        if (request.url === '/first') {
            response.writeHead(302, { Location: 'http://asset.invalid/final' }).end();
        } else response.end('redirected');
    }));
    const relay = await proxy(t, origin.port);
    await withEnvironment({ HTTP_PROXY: relay.url }, async () => {
        assert.equal(await get('http://release.invalid/first'), 'redirected');
    });
    assert.deepEqual(relay.tunnels, ['release.invalid:80', 'asset.invalid:80']);
});

test('proxy failure never falls back to a reachable origin', async t => {
    let requests = 0;
    const origin = await listen(t, createServer((_request, response) => { requests++; response.end('unexpected'); }));
    const relay = await proxy(t, null);
    await withEnvironment({ HTTP_PROXY: relay.url }, async () => assert.rejects(get(origin.url)));
    assert.equal(requests, 0);
});

test('cancellation after headers destroys the owned proxy transport', async t => {
    const origin = await listen(t, createServer((_request, response) => {
        response.writeHead(200);
        response.write('unfinished');
    }));
    const relay = await proxy(t, origin.port);
    await withEnvironment({ HTTP_PROXY: relay.url }, async () => {
        const stop = new AbortController();
        await assert.rejects(withDownloadFetch(undefined, async fetcher => {
            const response = await fetcher('http://download.invalid/slow', { signal: stop.signal });
            const reader = response.body.getReader();
            assert.equal((await reader.read()).done, false);
            stop.abort(new Error('cancelled streamed download'));
            await reader.read();
        }), /cancelled streamed download|aborted/);
    });
    // Network teardown is asynchronous at the receiving socket, but must finish.
    for (let attempt = 0; relay.sockets.size && attempt < 100; attempt++) {
        await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.equal(relay.sockets.size, 0);
});

async function archiveFixture(root) {
    const name = 'simplex-worker-v0.5.5-linux-x86_64-glibc2.34';
    const tree = join(root, name);
    for (const folder of ['bin/prompts/operations', 'bin/schemas/process', 'bin/plugins/llm', 'lib', 'third_party_licenses']) {
        await mkdir(join(tree, folder), { recursive: true });
    }
    for (const file of ['bin/config.example.yaml', 'bin/prompts/coding_agent.yaml', 'bin/prompts/operations/compact.yaml',
        'bin/schemas/process/skill.yaml', ...['run_command', 'spawn_process', 'read_process', 'send_process', 'poll_process']
            .map(tool => `bin/schemas/process/${tool}.yaml`), 'bin/plugins/llm/libllm_fixture.so', 'lib/runtime.so', 'LICENSE', 'README.md']) {
        await writeFile(join(tree, file), 'fixture');
    }
    for (const file of ['simplex', 'simplex_worker']) {
        await writeFile(join(tree, 'bin', file), '#!/bin/bash\nexit 0\n', { mode: 0o755 });
    }
    const archive = join(root, 'worker.tar.gz');
    await tar.c({ file: archive, gzip: true, cwd: root }, [name]);
    const bytes = await readFile(archive);
    return { archive: name + '.tar.gz', bytes };
}

test('default HTTPS downloads use environment proxy for metadata, checksums, archive and redirect', { timeout: 20000 }, async t => {
    const root = await mkdtemp(join(tmpdir(), 'simplex-proxy-test-'));
    t.after(() => rm(root, { recursive: true, force: true }));
    const key = join(root, 'key.pem');
    const cert = join(root, 'cert.pem');
    // Test-only trust is passed to the child with NODE_EXTRA_CA_CERTS. TLS
    // certificate and hostname verification remain enabled throughout.
    execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
        '-keyout', key, '-out', cert, '-subj', '/CN=installer-loopback-test',
        '-addext', 'subjectAltName=DNS:api.github.com,DNS:github.com,DNS:release-assets.githubusercontent.com'],
    { stdio: 'ignore' });
    const fixture = await archiveFixture(root);
    const prefix = 'https://github.com/Hazer-BJTU/simplex-cpp/releases/download/v0.5.5/';
    const metadata = { draft: false, prerelease: false, tag_name: 'v0.5.5', assets:
        [fixture.archive, 'SHA256SUMS'].map(name => ({ name, browser_download_url: prefix + name })) };
    const checksum = createHash('sha256').update(fixture.bytes).digest('hex') + '  ' + fixture.archive + '\n';
    const requests = [];
    const origin = await listen(t, createHttpsServer({ key: await readFile(key), cert: await readFile(cert) }, (request, response) => {
        requests.push(`${request.headers.host}${request.url}`);
        if (request.url.startsWith('/repos/')) response.end(JSON.stringify(metadata));
        else if (request.url.endsWith('/SHA256SUMS')) response.end(checksum);
        else if (request.url.endsWith('/' + fixture.archive)) {
            response.writeHead(302, { Location: 'https://release-assets.githubusercontent.com/fixture.tar.gz' }).end();
        } else if (request.url === '/fixture.tar.gz') response.end(fixture.bytes);
        else response.writeHead(404).end();
    }));
    const relay = await proxy(t, origin.port);
    const decoy = await proxy(t, null);
    const commandModule = new URL('../src/install/command.ts', import.meta.url).href;
    const program = `
        import {installWorker} from ${JSON.stringify(commandModule)};
        const code = await installWorker(['--platform','linux-amd64-standard','--directory',process.argv[1],
            '--version','v0.5.5','--no-update-path'], {checkHost:async()=>{},checkStartup:async()=>{},interactive:false});
        process.exitCode = code;
    `;
    for (const lower of [false, true]) {
        const home = join(root, lower ? 'lower-home' : 'upper-home');
        await mkdir(home);
        const env = environment({ HOME: home, NODE_EXTRA_CA_CERTS: cert, HTTP_PROXY: decoy.url,
            HTTPS_PROXY: lower ? decoy.url : relay.url, ...(lower ? { https_proxy: relay.url } : {}) });
        const result = await exec(process.execPath, ['--input-type=module', '-e', program, join(home, 'worker')],
            { env, timeout: 8000 });
        assert.match(result.stdout, /Installed worker 0.5.5 successfully/);
        const installed = JSON.parse(await readFile(join(home, 'worker/.simplex-worker-install.json'), 'utf8'));
        assert.equal(installed.platform, 'linux-amd64-standard');
    }
    assert.equal(requests.filter(url => url.startsWith('api.github.com/')).length, 2);
    assert.equal(requests.filter(url => url.endsWith('/SHA256SUMS')).length, 2);
    assert.equal(requests.filter(url => url.endsWith('/' + fixture.archive)).length, 2);
    assert.equal(requests.filter(url => url.startsWith('release-assets.githubusercontent.com/')).length, 2);
    assert.equal(decoy.tunnels.length, 0);
    for (const target of ['api.github.com:443', 'github.com:443', 'release-assets.githubusercontent.com:443']) {
        assert.ok(relay.tunnels.includes(target), target);
    }
    // Without the explicitly trusted test CA the HTTPS request must fail.
    const home = join(root, 'untrusted-home');
    await mkdir(home);
    const untrusted = environment({ HOME: home, HTTP_PROXY: relay.url, HTTPS_PROXY: relay.url });
    delete untrusted.NODE_EXTRA_CA_CERTS;
    await assert.rejects(exec(process.execPath, ['--input-type=module', '-e', program, join(home, 'worker')],
        { env: untrusted, timeout: 8000 }), /certificate|self-signed/i);
    assert.deepEqual(await readdir(home), []);
});
