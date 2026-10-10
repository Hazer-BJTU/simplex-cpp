/** An operator-owned data root can use links; its managed subtree cannot. */
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, readFileSync, realpathSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { it } from 'node:test';
import { startTestHub } from './helpers/hub.js';
import { ownedPath, removeChildDirectory, writePrivate } from '../src/subagents/storage.ts';

for (const missing of [false, true]) {
    it(`starts with a linked data-root ancestor and ${missing ? 'missing' : 'existing'} root`, async t => {
        const scratch = mkdtempSync(join(tmpdir(), 'simplex-root-link-'));
        let ctx;
        t.after(async () => {
            await ctx?.hub.stop();
            rmSync(scratch, { recursive: true, force: true });
        });
        const actual = join(scratch, 'actual'); mkdirSync(actual);
        const link = join(scratch, 'home'); symlinkSync(actual, link, 'dir');
        const dataDir = join(link, 'nested', 'hub');
        if (!missing) mkdirSync(dataDir, { recursive: true });
        ctx = await startTestHub({ dataDir });
        assert.equal(ctx.hub.config.dataDir, realpathSync(dataDir));
        const created = await fetch(`${ctx.base}/api/sessions`, { method: 'POST',
            headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ session: 'root-test' }) });
        assert.equal(created.status, 201);
        ctx.hub.panel.hooks.onProcessChange(ctx.hub.registry.require('root-test'));
        assert.equal(JSON.parse(readFileSync(join(actual, 'nested/hub/hub.json'), 'utf8')).sessions[0].id, 'root-test');
        const external = join(scratch, 'external'); mkdirSync(external);
        writeFileSync(join(external, 'keep'), 'outside-owned-storage');
        const id = 'subagent-12345678-1234-4123-8123-123456789abc';
        const children = join(ctx.hub.config.dataDir, 'subagents'); mkdirSync(children);
        symlinkSync(external, join(children, id), 'dir');
        assert.throws(() => ownedPath(ctx.hub.config.dataDir, id, 'metadata.json'), /unsafe directory/);
        assert.throws(() => removeChildDirectory(ctx.hub.config.dataDir, id), /unsafe directory/);
        assert.throws(() => writePrivate(join(children, id, 'metadata.json'), {}, 4096), /unsafe directory/);
        assert.equal(readFileSync(join(external, 'keep'), 'utf8'), 'outside-owned-storage');
        rmSync(join(children, id));
        rmSync(children, { recursive: true });
        symlinkSync(external, children, 'dir');
        assert.throws(() => ownedPath(ctx.hub.config.dataDir, id, 'metadata.json'), /unsafe directory/);
        assert.equal(readFileSync(join(external, 'keep'), 'utf8'), 'outside-owned-storage');
        rmSync(children);
    });
}

it('boots with recovery explicitly blocked when the managed subagents root is linked', async t => {
    const scratch = mkdtempSync(join(tmpdir(), 'simplex-blocked-root-'));
    let ctx;
    t.after(async () => { await ctx?.hub.stop(); rmSync(scratch, { recursive: true, force: true }); });
    const dataDir = join(scratch, 'hub'); mkdirSync(dataDir);
    const outside = join(scratch, 'outside'); mkdirSync(outside);
    writeFileSync(join(outside, 'keep'), 'possibly-owned-process-data');
    symlinkSync(outside, join(dataDir, 'subagents'), 'dir');
    ctx = await startTestHub({ dataDir });
    const response = await fetch(`${ctx.base}/api/subagents/recovery`);
    assert.equal(response.status, 200);
    const status = await response.json();
    assert.equal(status.blocked, true);
    assert.match(status.reason, /ownership retained/);
    assert.equal(readFileSync(join(outside, 'keep'), 'utf8'), 'possibly-owned-process-data');
    assert.equal(ctx.hub.subagents.children.size, 0);
    await assert.rejects(ctx.hub.supervisor.start(ctx.hub.registry.create('new-worker')), /ownership recovery is blocked/);
});
