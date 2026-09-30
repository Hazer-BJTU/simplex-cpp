import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { after, test } from 'node:test';
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
