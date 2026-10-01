import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { chmodSync, existsSync, mkdtempSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, test } from 'node:test';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { registryIntegrity, verifyPublished } from '../scripts/publish-release.mjs';

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

test('publishes a relative archive as an absolute file path', async t => {
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
fs.writeFileSync(process.env.FAKE_PUBLISHED, 'yes');
`);
    chmodSync(npm, 0o755);
    const manifest = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8'));
    const expected = `sha512-${createHash('sha512').update('packed bytes').digest('base64')}`;
    const server = createServer((request, response) => {
        assert.equal(request.url, `/${encodeURIComponent(manifest.name)}/${manifest.version}`);
        if (!existsSync(flag)) {
            response.writeHead(404).end();
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
                PATH: `${bin}:${process.env.PATH}`,
            },
        });
        let error = '';
        child.stderr.on('data', data => { error += data; });
        child.on('close', code => resolve({ code, error }));
    });
    assert.equal(result.code, 0, result.error);
    assert.equal(existsSync(flag), true);
});
