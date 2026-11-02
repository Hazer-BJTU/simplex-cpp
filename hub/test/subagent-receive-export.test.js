import assert from 'node:assert/strict';
import { test } from 'node:test';
import { ReceiveExports, snapshotMarkdown } from '../src/subagents/receive-export.ts';

const message = text => ({ content: [{ type: 'text', modality: 'text', raw: text }],
    reasoning: { raw: 'PRIVATE_REASONING' }, invokes: [{ arguments: 'PRIVATE_ARGUMENTS' }] });
const state = text => ({ turns: [{ user_input: message('user goal'),
    agent_loop_step: [{ model_response: message(text), invoke_returns: ['PRIVATE_TOOL_RESULT'] }] }],
    system_prompt: 'PRIVATE_SYSTEM', extras: 'PRIVATE_EXTRAS' });

test('saved dialogue is complete, marked and excludes reasoning/tools/private state', () => {
    const text = 'long response 🌻\n'.repeat(20000);
    const result = snapshotMarkdown(state(text));
    assert.ok(result.text.includes(text));
    assert.ok(result.text.includes('user goal'));
    assert.ok(!result.text.includes('PRIVATE_'));
    assert.ok(result.text.includes('Running work may not yet be saved'));
    assert.equal(result.blocks, 2);
    assert.ok(result.text.includes(`<!-- ${result.latest}:begin -->`));
    assert.ok(result.text.includes(`<!-- ${result.latest}:end -->`));
});

test('unsupported content is explicitly omitted; empty and invalid snapshots are distinct', () => {
    const input = state('answer');
    input.turns[0].user_input.content.push({ type: 'binary', raw: 'PRIVATE_BINARY' });
    const result = snapshotMarkdown(input);
    assert.equal(result.omitted, true);
    assert.ok(!result.text.includes('PRIVATE_BINARY'));
    assert.equal(snapshotMarkdown({ turns: [] }).blocks, 0);
    assert.throws(() => snapshotMarkdown(null), /no saved conversation/);
    assert.throws(() => snapshotMarkdown({ turns: [{}] }), /invalid saved turn/);
});

test('export pages stay fixed as source changes, preserve UTF-8, and bind to the owner', async t => {
    const cache = new ReceiveExports(); t.after(() => cache.close());
    const input = state('🌻'.repeat(30000));
    let checks = 0;
    let page = await cache.start('parent/lifecycle/run/child', async () => input, () => checks++);
    const first = page;
    assert.throws(() => cache.page('another-owner', { token: page.token, offset: page.next }), /belongs/);
    assert.throws(() => cache.page('parent/lifecycle/run/child', { token: page.token, offset: -1 }), /offset/);
    input.turns[0].agent_loop_step[0].model_response = message('NEW_SNAPSHOT');
    let collected = '';
    for (;;) {
        assert.ok(Buffer.byteLength(page.text) <= 32768);
        assert.ok(!page.text.includes('�'));
        assert.equal(Buffer.byteLength(collected), page.offset);
        collected += page.text;
        if (page.done) break;
        page = cache.page('parent/lifecycle/run/child', { token: page.token, offset: page.next });
    }
    assert.equal(Buffer.byteLength(collected), first.total);
    assert.ok(collected.includes('🌻'.repeat(30000)));
    assert.ok(!collected.includes('NEW_SNAPSHOT'));
    assert.equal(checks, 1);
    assert.throws(() => cache.page('parent/lifecycle/run/child', { token: page.token, offset: 0 }), /expired/);
});

test('pending reads reserve capacity, revalidate ownership and expire retained exports', async t => {
    const cache = new ReceiveExports(); t.after(() => cache.close());
    let release;
    const wait = new Promise(resolve => { release = resolve; });
    const first = cache.start('a', () => wait, () => { throw new Error('authorization expired'); });
    const second = cache.start('b', () => wait, () => {});
    await assert.rejects(cache.start('c', () => wait, () => {}), /capacity/);
    release(state('x'.repeat(40000)));
    await assert.rejects(first, /authorization expired/);
    const page = await second;
    const now = Date.now();
    t.mock.method(Date, 'now', () => now + 120001);
    assert.throws(() => cache.page('b', { token: page.token, offset: page.next }), /expired/);
});
