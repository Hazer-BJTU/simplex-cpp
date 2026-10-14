/** Real-Hub example checks executed outside the repository against the packed SDK. */
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { WebSocket } from 'ws';
import { createServer, build } from 'vite';

const [consumer, installed] = process.argv.slice(2);
const load = path => import(pathToFileURL(path));
const { createHub } = await load(join(installed, 'dist/src/hub.js'));
const { loadConfig, hubRoot } = await load(join(installed, 'dist/src/config.js'));
const { createLogger } = await load(join(installed, 'dist/src/log.js'));
const { discover, observe } = await load(join(consumer, 'examples/observer.mjs'));
const { runTerminal } = await load(join(consumer, 'examples/tui/main.mjs'));
const { mountViewer } = await load(join(consumer, 'examples/web/main.mjs'));
const dataDir = mkdtempSync(join(tmpdir(), 'simplex-sdk-example-hub-'));
const stops = [];
const sockets = [];
const proxies = [];
let hub;
const token = 'example-test-token';
async function until(predicate) {
    const deadline = performance.now() + 5000;
    while (!predicate()) { if (performance.now() > deadline) throw new Error('Example condition timed out'); await delay(10); }
}
async function start() {
    const { config } = loadConfig({ overrides: { listen: { host: '127.0.0.1', port: 0 }, toolRequests: { port: 0 },
        panel: { token }, dataDir, limits: { transcriptEvents: 16 } } });
    hub = createHub({ config, hubRoot, log: createLogger({ level: 'silent' }), version: 'sdk-test' });
    const address = await hub.start();
    return `http://127.0.0.1:${address.port}`;
}
function fixtureDocument() {
    const elements = new Map(['token', 'session', 'output', 'connect'].map(id => [id, {
        value: id === 'token' ? token : id === 'session' ? 'example' : '', textContent: '',
        addEventListener(_name, callback) { this.click = callback; },
    }]));
    return { querySelector: selector => elements.get(selector.slice(1)) };
}
try {
    const base = await start();
    const session = hub.registry.create('example');
    const worker = new WebSocket(`${base.replace('http:', 'ws:')}/agent/example/events?token=${session.token}`);
    sockets.push(worker);
    await new Promise((resolve, reject) => { worker.once('open', resolve); worker.once('error', reject); });
    // A controlled worker produces enough retained data for multiple 512 KiB
    // replay pages. The ring discards the oldest entries to exercise gaps too.
    for (let sequence = 1; sequence <= 20; ++sequence) worker.send(JSON.stringify({
        type: 'event', event: 'model_response', session_id: 'example', worker_id: 'fixture',
        request_id: 'r', run_id: 'run', sequence,
        data: { content: [{ type: 'text', modality: 'text', raw: 'x'.repeat(60000) }] },
    }));
    await until(() => session.stats.events === 20);
    assert.equal((await discover(base, token))[0].session_id, 'example');
    await assert.rejects(discover(base, 'wrong'), /401/);
    let pages = 0;
    let controls = 0;
    class ObservedSocket extends WebSocket {
        constructor(url) {
            super(url); sockets.push(this);
            this.on('message', raw => {
                const value = JSON.parse(raw);
                if (value.type === 'subscribed') ++pages;
                if (value.type === 'event' && value.envelope?.event === 'history') ++controls;
            });
        }
        addEventListener(name, listener) {
            if (name !== 'message') return super.addEventListener(name, listener);
            super.addEventListener(name, event => {
                const value = JSON.parse(event.data);
                if (value.type === 'subscribed') listener({ data: JSON.stringify({ ...value, request_id: 'superseded' }) });
                listener(event);
            });
        }
    }
    const views = [];
    stops.push(observe(base, token, 'example', view => views.push(view), ObservedSocket));
    await until(() => views.at(-1)?.status === 'Live');
    assert.ok(pages > 1, 'must exercise multiple correlated pages');
    assert.equal(views.at(-1).cursor, 20);
    assert.equal(views.at(-1).incomplete, true);
    assert.equal(views.at(-1).lines.length, 16);
    worker.send(JSON.stringify({ type: 'event', event: 'history', session_id: 'example', worker_id: 'fixture',
        request_id: 'other-panel-query', run_id: 'run', sequence: 21,
        data: { request_id: 'other-panel-query', revision: 1, start: 0, step: 0, next: 0, next_step: 0, total: 0, turns: [] } }));
    await until(() => session.stats.events === 21);
    await until(() => controls > 0);
    assert.equal(views.at(-1).status, 'Live');
    assert.equal(views.at(-1).cursor, 20);

    let terminal = '';
    stops.push(await runTerminal(base, 'example', token, text => { terminal += text; }, ObservedSocket));
    await until(() => terminal.includes('Live'));
    assert.match(terminal, /incomplete/);

    const proxy = await createServer({ configFile: false, root: join(consumer, 'examples/web'), logLevel: 'silent',
        server: { host: '127.0.0.1', port: 0, proxy: {
            '/api': { target: base }, '/panel/ws': { target: base, ws: true },
        } } });
    proxies.push(proxy); await proxy.listen();
    const origin = `http://127.0.0.1:${proxy.httpServer.address().port}`;
    assert.match(await (await fetch(origin)).text(), /Simplex SDK read-only example/);
    class BrowserSocket extends WebSocket {
        constructor(url) { super(url, { origin }); sockets.push(this); }
    }
    const doc = fixtureDocument();
    stops.push(mountViewer(doc, origin, BrowserSocket));
    await doc.querySelector('#connect').click();
    await until(() => doc.querySelector('#output').textContent.startsWith('Live'));
    assert.match(doc.querySelector('#output').textContent, /incomplete retained preview/);
    // Optional local/browser-job check; normal npm validation needs no browser download.
    if (process.env.SDK_EXAMPLE_BROWSER === '1') {
        const { chromium } = await import('@playwright/test');
        const browser = await chromium.launch();
        try {
            const page = await browser.newPage();
            await page.goto(origin);
            await page.locator('#token').fill(token);
            await page.locator('#session').fill('example');
            await page.locator('#connect').click();
            await page.waitForFunction(() => document.querySelector('#output').textContent.startsWith('Live'));
            assert.match(await page.locator('#output').textContent(), /incomplete retained preview/);
            await page.locator('#connect').click();
            await page.waitForFunction(() => document.querySelector('#output').textContent.startsWith('Live'));
        } finally { await browser.close(); }
    }
    for (const [presented, requestedOrigin, status] of [[token, 'http://other.invalid', 403], ['wrong', origin, 401]]) {
        const rejected = new WebSocket(`${origin.replace('http:', 'ws:')}/panel/ws?token=${presented}`, { origin: requestedOrigin });
        sockets.push(rejected);
        assert.equal(await new Promise(resolve => {
            rejected.once('unexpected-response', (_req, response) => { response.resume(); rejected.terminate(); resolve(response.statusCode); });
            rejected.on('error', () => {});
        }), status);
    }

    const previousEpoch = views.at(-1).epoch;
    for (const stop of stops.splice(0)) stop();
    for (const socket of sockets) socket.terminate();
    await hub.stop();
    const restarted = await start();
    const replay = [];
    stops.push(observe(restarted, token, 'example', view => replay.push(view), ObservedSocket));
    await until(() => replay.at(-1)?.status === 'Live');
    assert.notEqual(replay.at(-1).epoch, previousEpoch);
    assert.equal(replay.at(-1).cursor, 0);
    assert.deepEqual(replay.at(-1).lines, []);

    // Mutate only the test socket's incoming welcome, preserving real transport.
    class OlderSocket extends WebSocket {
        constructor(url) {
            super(url); sockets.push(this);
        }
        addEventListener(name, listener) {
            if (name !== 'message') return super.addEventListener(name, listener);
            super.addEventListener(name, event => {
                const value = JSON.parse(event.data);
                if (value.type === 'welcome') value.hub.capabilities = [];
                listener({ data: JSON.stringify(value) });
            });
        }
    }
    const unsupported = [];
    stops.push(observe(restarted, token, 'example', view => unsupported.push(view), OlderSocket));
    await until(() => unsupported.some(view => view.status === 'Unsupported replay capabilities'));
    await build({ configFile: false, root: join(consumer, 'examples/web'), base: './', logLevel: 'warn' });
    console.log('Packed SDK examples: real Hub, fixture worker, paged/gapped replay, auth, proxy, epoch restart and unsupported capabilities passed');
} finally {
    for (const stop of stops) stop();
    for (const socket of sockets) socket.terminate();
    for (const proxy of proxies) await proxy.close();
    if (hub) await hub.stop();
    rmSync(dataDir, { recursive: true, force: true });
}
