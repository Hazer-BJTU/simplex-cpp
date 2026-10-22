/** Imported from an unpacked package with no dependencies installed. */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import net from 'node:net';
import http from 'node:http';
import https from 'node:https';
import { syncBuiltinESMExports } from 'node:module';

const cwd = process.cwd();
const environment = { ...process.env };
const events = new Map(process.eventNames().map(name => [name, process.listeners(name)]));
const fail = () => { throw new Error('SDK import attempted a side effect'); };
for (const name of ['setTimeout', 'setInterval', 'setImmediate', 'fetch', 'WebSocket']) globalThis[name] = fail;
for (const target of [fs, fsp]) {
    for (const name of ['writeFile', 'writeFileSync', 'appendFile', 'appendFileSync', 'mkdir', 'mkdirSync',
        'rm', 'rmSync', 'rename', 'renameSync', 'createWriteStream']) {
        if (name in target) target[name] = fail;
    }
}
net.Socket.prototype.connect = fail;
net.Server.prototype.listen = fail;
http.request = https.request = http.get = https.get = fail;
process.chdir = fail;
process.on = process.addListener = process.once = process.prependListener = fail;
syncBuiltinESMExports();
const sdk = await import('@hazer-bjtu/simplex-hub/ui');
assert.deepEqual(Object.keys(sdk).sort(), [
    'CAPABILITIES', 'ERROR_CODES', 'HUB_MESSAGE_TYPES', 'PANEL_MESSAGE_TYPES',
    'PANEL_PROTOCOL', 'PANEL_SOCKET_PATH', 'PANEL_VERSION', 'REST_ROUTES', 'SESSIONLESS_MESSAGE_TYPES',
].sort());
assert.equal(sdk.PANEL_VERSION, 1);
assert.equal(sdk.PANEL_SOCKET_PATH, '/panel/ws');
assert.equal(sdk.REST_ROUTES.length, 31);
assert.equal(process.cwd(), cwd);
assert.deepEqual({ ...process.env }, environment);
assert.deepEqual(new Map(process.eventNames().map(name => [name, process.listeners(name)])), events);
for (const value of Object.values(sdk)) assert.notEqual(typeof value, 'function');
for (const path of ['@hazer-bjtu/simplex-hub', '@hazer-bjtu/simplex-hub/dist/src/hub.js']) {
    await assert.rejects(import(path), { code: 'ERR_PACKAGE_PATH_NOT_EXPORTED' });
}
