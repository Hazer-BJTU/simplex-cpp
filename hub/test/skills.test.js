import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm, symlink, mkdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { SkillStore, parseSkill } from '../src/skills/store.ts';
import { expandSkills, skillMarkers, skillCompletion, referencedSkillIds } from '../web/src/app/skillInput.ts';
import { createPanelStore } from '../web/src/state/store.ts';

const source = 'name: review\ntitle: Review\ndescription: Review changes.\ntext: |\n  First instruction.\n  Second instruction.\n';
const parsed = text => parseSkill('review', Buffer.from(text));

test('skill library seeds once, reads changes, isolates errors and bounds files', async t => {
    const dir = await mkdtemp(join(tmpdir(), 'simplex-skills-'));
    t.after(() => rm(dir, { recursive: true, force: true }));
    const store = new SkillStore(dir);
    assert.equal((await store.list()).skills[0].id, 'code-review');
    await writeFile(join(store.directory, 'code-review.yaml'), source);
    assert.equal((await store.read('code-review')).name, 'review');
    const first = await store.read('code-review');
    await writeFile(join(store.directory, 'code-review.yaml'), source.replace('First', 'Changed'));
    assert.notEqual((await store.read('code-review')).revision, first.revision);
    await writeFile(join(store.directory, 'broken.yaml'), 'text: [');
    await writeFile(join(store.directory, 'large.yaml'), Buffer.alloc(65537));
    await symlink(join(store.directory, 'code-review.yaml'), join(store.directory, 'link.yaml'));
    await mkdir(join(store.directory, 'folder.yaml'));
    const result = await store.list();
    assert.equal(result.skills.length, 1);
    assert.deepEqual(result.errors.map(e => e.id), ['broken', 'folder', 'large', 'link']);
    await assert.rejects(store.read('../review'), /Invalid skill ID/);
    await assert.rejects(store.read('missing'), /not found/);
    await assert.rejects(store.read('link'), /regular file/);
});

test('skill library refuses a symlink directory without writing a template', async t => {
    const dir = await mkdtemp(join(tmpdir(), 'simplex-skills-'));
    t.after(() => rm(dir, { recursive: true, force: true }));
    await mkdir(join(dir, 'outside'));
    await symlink(join(dir, 'outside'), join(dir, 'skills'));
    await assert.rejects(new SkillStore(dir).list(), /regular directory/);
});

test('skill validation rejects ambiguous and malformed instruction documents', () => {
    for (const text of ['', '[]', source + 'name: duplicate', source + 'unknown: true',
        source.replace('name: review', 'name: 3'), source.replace('Review changes.', '""'),
        source + 'keywords: [3]', source.replace('name: review', 'name: &a review') + 'keywords: [*a]']) {
        assert.throws(() => parsed(text));
    }
    assert.throws(() => parseSkill('bad', Buffer.from([0xff])), /UTF-8/);
    assert.throws(() => parsed(source + '\0'), /NUL/);
});

test('markers expand at their exact position without recursive interpretation', () => {
    const doc = parsed(source);
    assert.equal(expandSkills('before @{skill:review} after', { review: doc }), `before ${doc.text} after`);
    assert.equal(expandSkills('@{skill:review}@{skill:review}', { review: doc }), doc.text.repeat(2));
    assert.equal(expandSkills('\\@{skill:missing}', {}), '@{skill:missing}');
    assert.equal(expandSkills('@{skill:review}', { review: { ...doc, text: '@{skill:other}' } }), '@{skill:other}');
    for (const draft of ['@{skill:missing}', '@{skill:review', '@{skill:../review}', '@{skill:constructor}']) {
        assert.throws(() => expandSkills(draft, { review: doc }));
    }
    assert.throws(() => expandSkills('@{skill:review}'.repeat(17), { review: doc }), /16/);
    assert.throws(() => expandSkills('@{skill:review}'.repeat(5), { review: { ...doc, text: 'x'.repeat(65536) } }), /256 KiB/);
    assert.deepEqual(skillCompletion('do @{skill:rev', 14), { start: 3, query: 'rev' });
    assert.equal(skillCompletion('\\@{skill:rev', 12), null);
    assert.deepEqual(skillMarkers('plain'), []);
});

test('explicit input rejection recovers marker draft and its exact snapshots', () => {
    const store = createPanelStore();
    const recovery = { draft: '@{skill:review} request', skills: { review: parsed(source) } };
    const parts = [{ type: 'text', raw: expandSkills(recovery.draft, recovery.skills) }];
    store.getState().beginInput('demo', 'request-1', parts, 'message', recovery);
    store.getState().failInput('demo', 'request-1', 'refused');
    assert.deepEqual(store.getState().failedInput.recovery, recovery);
    assert.equal(store.getState().failedInput.parts[0].raw, parts[0].raw);
});

test('skill REST routes require panel authentication and return documented snapshots', async t => {
    const { startTestHub } = await import('./helpers/hub.js');
    const ctx = await startTestHub({ panel: { token: 'skill-test-token' } });
    t.after(async () => { await ctx.hub.stop(); await rm(ctx.config.dataDir, { recursive: true, force: true }); });
    const base = ctx.base;
    for (const path of ['/api/skills', '/api/skills/template', '/api/skills/code-review']) {
        assert.equal((await fetch(base + path)).status, 401);
        const response = await fetch(base + path, { headers: { authorization: 'Bearer skill-test-token' } });
        assert.equal(response.status, 200);
        const body = await response.json();
        if (path === '/api/skills') {
            assert.equal(body.skills[0].id, 'code-review');
            assert.equal(Object.hasOwn(body.skills[0], 'text'), false);
        } else assert.equal(typeof body.text, 'string');
    }
});

test('concurrent first reads share initialization; explicit limits and template reservation hold', async t => {
    const dir = await mkdtemp(join(tmpdir(), 'simplex-skills-'));
    t.after(() => rm(dir, { recursive: true, force: true }));
    const store = new SkillStore(dir);
    const results = await Promise.all(Array.from({ length: 8 }, () => store.read('code-review')));
    assert.equal(new Set(results.map(item => item.revision)).size, 1);
    await assert.rejects(store.read('template'), /Invalid skill ID/);
    await Promise.all(Array.from({ length: 256 }, (_, i) => writeFile(join(store.directory, `file-${i}.yaml`), source)));
    await assert.rejects(store.list(), /256 files/);
});

test('reference discovery retains complete markers through invalid draft edits', () => {
    for (const invalid of ['@{skill:', '@{skill:bad id}']) {
        assert.deepEqual(referencedSkillIds(`@{skill:review} ${invalid}`), ['review']);
        assert.deepEqual(referencedSkillIds(`${invalid} @{skill:review}`), ['review']);
    }
    assert.deepEqual(referencedSkillIds('@{skill:review} @{skill:review}'), ['review']);
    assert.deepEqual(referencedSkillIds('\\@{skill:review}'), []);
    assert.deepEqual(referencedSkillIds('removed'), []);
    assert.deepEqual(referencedSkillIds('@{skill:review} '.repeat(17)), ['review']);
    assert.throws(() => skillMarkers('@{skill:review} '.repeat(17)), /at most 16/);
});
