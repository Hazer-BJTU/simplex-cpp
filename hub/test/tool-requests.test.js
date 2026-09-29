/** Dedicated worker tool transport: no executable routes in this release. */
import assert from 'node:assert/strict';
import { createServer } from 'node:net';
import { afterEach, it } from 'node:test';
import { createHub } from '../src/hub.ts';
import { hubRoot, validateConfig } from '../src/config.ts';
import { createLogger } from '../src/log.ts';
import { startTestHub, testConfig } from './helpers/hub.js';
import { connectWorker, upgradeStatus } from './helpers/worker.js';

const hubs = [];
afterEach(async () => {
    for (const hub of hubs.splice(0)) await hub.stop();
});

async function setup(overrides = {}) {
    const ctx = await startTestHub(overrides);
    hubs.push(ctx.hub);
    const session = ctx.hub.registry.create('tools-test', {});
    const endpoints = ctx.hub.supervisor.endpointsFor(session.id, session.token);
    const url = new URL(endpoints.tools);
    url.pathname += '/files/read';
    return { ...ctx, session, endpoints, url };
}

function request(extra = {}) {
    return {
        type: 'tool_request',
        data: {
            worker_id: 'worker-1', session_id: 'tools-test', run_id: 'run-1',
            request_id: 'request-1', arguments: {}, ...extra,
        },
    };
}

it('rejects every route with a correlated not_implemented response without executing arguments', async () => {
    const ctx = await setup();
    assert.notEqual(new URL(ctx.endpoints.events).port, ctx.url.port);
    for (const route of ['files/read', 'run_command', 'future-operation']) {
        ctx.url.pathname = `/agent/tools-test/tools/${route}`;
        const peer = await connectWorker(ctx.url.href);
        peer.send(request({ arguments: { command: 'must not execute', future: [] } }));
        const response = await peer.waitFor((value) => value.type === 'tool_response');
        assert.deepEqual(response, {
            type: 'tool_response',
            data: {
                worker_id: 'worker-1', session_id: 'tools-test', run_id: 'run-1',
                request_id: 'request-1', route, status: 'rejected',
                error: { code: 'not_implemented', message: 'remote tool route is not implemented' },
            },
        });
        assert.equal((await peer.waitForClose()).code, 1000);
        assert.equal(peer.messages.length, 1);
    }
    assert.equal(ctx.session.prompts.size, 0);
    assert.equal(ctx.session.connection, null);
});

it('authenticates sessions and separates tool, panel and event listeners', async () => {
    const ctx = await setup();
    const url = new URL(ctx.url);
    url.search = '';
    assert.equal(await upgradeStatus(url.href), 401);
    url.search = '?token=wrong';
    assert.equal(await upgradeStatus(url.href), 401);
    url.pathname = '/agent/absent/tools/files/read';
    assert.equal(await upgradeStatus(url.href), 404);
    assert.equal(await upgradeStatus(`${ctx.wsBase}${ctx.url.pathname}${ctx.url.search}`), 404);
    for (const path of ['/panel', '/agent/tools-test/events', '/agent/tools-test/confirm',
        '/agent/tools-test/tools', '/agent/tools-test/tools/Files', '/agent/tools-test/tools/file%2Fread']) {
        url.pathname = path;
        assert.equal(await upgradeStatus(url.href), 404);
    }
    const response = await fetch(`http://${ctx.url.host}/api/meta`);
    assert.equal(response.status, 404);
});

it('closes invalid envelopes and binary frames without a tool response', async () => {
    const ctx = await setup();
    const cases = [
        ['{', 1008], ['[]', 1008],
        [JSON.stringify({ ...request(), type: 'confirmation_request' }), 1008],
        [JSON.stringify(request({ session_id: 'other' })), 1008],
        [JSON.stringify(request({ arguments: [] })), 1008],
        [JSON.stringify(request({ request_id: '  ' })), 1008],
        [JSON.stringify(request({ worker_id: null })), 1008],
        [Buffer.from('{}'), 1003],
    ];
    for (const [frame, code] of cases) {
        const peer = await connectWorker(ctx.url.href);
        peer.ws.send(frame);
        assert.equal((await peer.waitForClose()).code, code);
        assert.equal(peer.messages.length, 0);
    }
    const invalidUtf8 = await connectWorker(ctx.url.href);
    invalidUtf8.ws.send(Buffer.from([0xc3, 0x28]), { binary: false });
    assert.equal((await invalidUtf8.waitForClose()).code, 1007);
    assert.equal(invalidUtf8.messages.length, 0);
});

it('rejects oversized frames and never dispatches a second frame', async () => {
    const ctx = await setup({ limits: { maxMessageBytes: 1024 } });
    const large = await connectWorker(ctx.url.href);
    large.ws.send('x'.repeat(2048));
    assert.equal((await large.waitForClose()).code, 1009);
    assert.equal(large.messages.length, 0);
    const repeated = await connectWorker(ctx.url.href);
    // Cork both frames so they are received before the response write callback.
    repeated.ws._socket.cork();
    repeated.send(request());
    repeated.send(request({ request_id: 'second' }));
    repeated.ws._socket.uncork();
    assert.equal((await repeated.waitForClose()).code, 1008);
    assert.equal(repeated.messages.length, 1);
    assert.equal(repeated.messages[0].data.request_id, 'request-1');
});

it('bounds idle connections and releases capacity after the deadline', async () => {
    const ctx = await setup({ toolRequests: { port: 0, timeoutMs: 750, maxConnections: 1 } });
    const idle = await connectWorker(ctx.url.href);
    assert.equal(await upgradeStatus(ctx.url.href), 503);
    assert.equal((await idle.waitForClose()).code, 1006);
    const next = await connectWorker(ctx.url.href);
    next.send(request());
    assert.equal((await next.waitFor((value) => value.type === 'tool_response')).data.status, 'rejected');
    await next.waitForClose();
});

it('terminates upgraded sockets on hub shutdown', async () => {
    const ctx = await setup();
    const peer = await connectWorker(ctx.url.href);
    await ctx.hub.stop();
    hubs.splice(hubs.indexOf(ctx.hub), 1);
    assert.equal((await peer.waitForClose()).code, 1006);
    assert.equal(ctx.hub.toolHttp.server.listening, false);
    assert.equal(ctx.hub.http.server.listening, false);
});

it('releases the main listener if the tool listener cannot bind', async () => {
    const occupied = createServer();
    await new Promise((resolve) => occupied.listen(0, '127.0.0.1', resolve));
    try {
        const config = testConfig({ toolRequests: { port: occupied.address().port } });
        const hub = createHub({ config, log: createLogger({ level: 'silent' }), hubRoot });
        await assert.rejects(hub.start(), { code: 'EADDRINUSE' });
        assert.equal(hub.http.server.listening, false);
        assert.equal(hub.toolHttp.server.listening, false);
    } finally {
        await new Promise((resolve) => occupied.close(resolve));
    }
});

it('validates the dedicated listener and resource limits', () => {
    for (const fields of [
        { port: -1 }, { port: 65536 }, { host: null }, { timeoutMs: 0 },
        { timeoutMs: 2147483648 }, { maxConnections: 0 }, { maxConnections: 1.5 },
    ]) {
        const config = testConfig();
        Object.assign(config.toolRequests, fields);
        assert.throws(() => validateConfig(config), /toolRequests/);
    }
    const config = testConfig();
    config.listen.port = 8800;
    config.toolRequests.port = 8800;
    assert.throws(() => validateConfig(config), /must differ/);
});

it('closes both listeners if a later mock startup fails', async () => {
    const occupied = createServer();
    await new Promise((resolve) => occupied.listen(0, '127.0.0.1', resolve));
    try {
        const config = testConfig({
            mock: { enabled: true, listen: `127.0.0.1:${occupied.address().port}` },
        });
        const hub = createHub({ config, log: createLogger({ level: 'silent' }), hubRoot });
        await assert.rejects(hub.start(), { code: 'EADDRINUSE' });
        assert.equal(hub.http.server.listening, false);
        assert.equal(hub.toolHttp.server.listening, false);
        assert.throws(() => hub.supervisor.endpointsFor('demo', 'token'), /not listening/);
    } finally {
        await new Promise((resolve) => occupied.close(resolve));
    }
});
