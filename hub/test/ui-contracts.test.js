import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import { parse } from '@babel/parser';
import * as ui from '../ui/index.ts';
import * as compatibility from '../shared/protocol.ts';
import { checkUiBoundary } from '../scripts/check-ui-boundary.mjs';
import { startTestHub } from './helpers/hub.js';

const root = fileURLToPath(new URL('../', import.meta.url));

test('contract constants are unchanged through compatibility imports; no runtime helpers escape', () => {
    for (const [key, value] of Object.entries(compatibility)) assert.strictEqual(ui[key], value);
    assert.deepEqual(Object.keys(ui).sort(), [
        'CAPABILITIES', 'ERROR_CODES', 'HUB_MESSAGE_TYPES', 'PANEL_MESSAGE_TYPES',
        'PANEL_PROTOCOL', 'PANEL_SOCKET_PATH', 'PANEL_VERSION', 'REST_ROUTES', 'SESSIONLESS_MESSAGE_TYPES',
    ].sort());
    assert.equal(ui.PANEL_SOCKET_PATH, '/panel/ws');
    assert.deepEqual(ui.PANEL_PROTOCOL, { name: 'simplex-hub-panel', version: 1 });
});

test('every live panel REST route has exactly one contract, and no invented route', async t => {
    const ctx = await startTestHub();
    t.after(async () => { await ctx.hub.stop(); rmSync(ctx.config.dataDir, { recursive: true, force: true }); });
    const found = ctx.hub.http.router.list().filter(route => route.pattern.startsWith('/api/'))
        .map(route => `${route.method} ${route.pattern}`);
    assert.equal(found.length, 26);
    assert.equal(new Set(found).size, found.length);
    assert.deepEqual([...ui.REST_ROUTES].sort(), found.sort());
    const contract = parse(readFileSync(join(root, 'ui/rest.ts'), 'utf8'), { sourceType: 'module', plugins: ['typescript'] });
    const table = contract.program.body.find(n => n.declaration?.id?.name === 'RestOperations').declaration;
    assert.deepEqual(table.body.body.map(n => n.key.value).sort(), found);
});

test('SDK graph stays contract-only, including declarations and unexported modules', () => {
    assert.equal(checkUiBoundary(join(root, 'ui')).size, 5);
});

for (const [name, code] of Object.entries({
    function: 'export function run() {}',
    class: 'export class Client {}',
    call: 'export const x = Object.freeze({});',
    getter: 'export const x = { get value() { return 1; } };',
    backImport: 'import type { HubConfig } from "../src/config.ts";',
    externalType: 'export type X = import("node:fs").Stats;',
    externalRuntime: 'import ws from "ws";',
    wildcard: 'export * from "./index.ts";',
    ambient: '/// <reference types="node" />\nexport type X = string;',
    global: 'export const x = Math.max;',
})) {
    test(`contract boundary rejects ${name}`, t => {
        const directory = mkdtempSync(join(tmpdir(), 'simplex-ui-boundary-'));
        t.after(() => rmSync(directory, { recursive: true, force: true }));
        writeFileSync(join(directory, 'index.ts'), 'export type Good = string;');
        writeFileSync(join(directory, 'hidden.d.ts'), code);
        assert.throws(() => checkUiBoundary(directory));
    });
}

test('representative actual REST replies retain revisions, URL previews, status distinctions and diagnostics', async t => {
    const ctx = await startTestHub();
    t.after(async () => { await ctx.hub.stop(); rmSync(ctx.config.dataDir, { recursive: true, force: true }); });
    async function request(path, method = 'GET', body) {
        const response = await fetch(`${ctx.base}${path}`, { method,
            ...(body === undefined ? {} : { headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) }) });
        return { status: response.status, body: await response.json() };
    }
    const metadata = await request('/api/meta');
    assert.deepEqual(metadata.body.protocol, ui.PANEL_PROTOCOL);
    assert.equal(metadata.status, 200);
    const template = await request('/api/configurations/launch/template');
    assert.equal(typeof template.body.text, 'string');
    const saved = await request('/api/configurations/launch/contract-test', 'PUT', { text: template.body.text, revision: null });
    assert.equal(saved.status, 200);
    assert.deepEqual(Object.keys(saved.body).sort(), ['id', 'kind', 'revision', 'text']);
    assert.equal(typeof saved.body.revision, 'string');
    const conflict = await request('/api/configurations/launch/contract-test', 'PUT', { text: template.body.text, revision: null });
    assert.equal(conflict.status, 409);
    assert.equal(typeof conflict.body.error, 'string');
    assert.equal(typeof conflict.body.message, 'string');
    const preview = await request('/api/configurations/preview', 'POST', { launch: template.body.text });
    assert.equal(preview.status, 200);
    for (const key of ['events', 'confirm', 'tools']) assert.equal(typeof preview.body.endpoints[key], 'string');
    assert.deepEqual((await request('/api/subagents/recovery')).body, { blocked: false, reason: null });
    assert.equal((await request('/api/sessions', 'POST', { session: 'contracts' })).status, 201);
    assert.deepEqual((await request('/api/sessions/contracts/events')).body, { session: 'contracts', since: 0, latest: 0, events: [] });
    const logs = await request('/api/sessions/contracts/logs');
    assert.deepEqual(logs.body, { session: 'contracts', lines: [], dropped: 0, log_path: null });
    const snapshot = (await request('/api/sessions/contracts/snapshot')).body;
    assert.equal(snapshot.session_id, 'contracts');
    assert.equal(snapshot.state, null);
    const invalidAnswer = await request('/api/sessions/contracts/answer', 'POST', {});
    assert.equal(invalidAnswer.status, 400);
    assert.equal(invalidAnswer.body.error, 'invalid_arguments');
    // A normal supervisor refusal is a result, not the common error envelope.
    const start = ctx.hub.supervisor.start;
    ctx.hub.supervisor.start = async () => ({ ok: false, error: 'already running' });
    t.after(() => { ctx.hub.supervisor.start = start; });
    assert.deepEqual(await request('/api/sessions/contracts/start', 'POST', {}), {
        status: 409, body: { ok: false, error: 'already running' },
    });
});

test('boundary scans emitted JavaScript and follows declaration dependencies', t => {
    const directory = mkdtempSync(join(tmpdir(), 'simplex-ui-emitted-'));
    t.after(() => rmSync(directory, { recursive: true, force: true }));
    writeFileSync(join(directory, 'index.js'), "export { VERSION } from './constants.js';");
    writeFileSync(join(directory, 'constants.js'), 'export const VERSION = 1;');
    writeFileSync(join(directory, 'index.d.ts'), "export { VERSION } from './constants.js';");
    writeFileSync(join(directory, 'constants.d.ts'), 'export declare const VERSION: 1;');
    assert.equal(checkUiBoundary(directory).size, 4);
    writeFileSync(join(directory, 'constants.js'), 'export const VERSION = (() => 1)();');
    assert.throws(() => checkUiBoundary(directory), /Executable/);
    writeFileSync(join(directory, 'constants.js'), 'export const VERSION = 1;');
    writeFileSync(join(directory, 'constants.d.ts'), 'export type Hidden = import("node:fs").Stats;');
    assert.throws(() => checkUiBoundary(directory), /External/);
});
