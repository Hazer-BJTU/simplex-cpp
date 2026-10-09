import assert from 'node:assert/strict';
import { it } from 'node:test';
import { normalizeDisplay } from '../src/protocol/display.ts';
import { parseCompactResult } from '../web/src/state/compact.ts';
import { createPanelStore } from '../web/src/state/store.ts';
import { buildRounds } from '../web/src/app/rounds.ts';
import { setupPanelHub } from './helpers/panel.js';
import { workerEvent } from './helpers/worker.js';
import { compactSummary } from './helpers/compact.js';

function result(summary) {
    return { summary, memory_file: '/memory/archive/state.md', durable: true,
        revision: 2, removed_turns: 1 };
}

for (const bytes of [8192, 32768]) {
    it(`preserves a ${bytes}-byte compact summary independently of diagnostic limits`, () => {
        const source = result(compactSummary(bytes));
        const projected = normalizeDisplay('compact_finished', source);
        assert.deepEqual(projected, source);
        assert.equal(parseCompactResult(projected).summary, source.summary);
        const automatic = normalizeDisplay('compact_finished', { ...source, origin: 'automatic', cycle: 1 });
        assert.equal(parseCompactResult(automatic).summary, source.summary);
        assert.equal(parseCompactResult(automatic).origin, 'automatic');
        const diagnostics = normalizeDisplay('compact_finished', {
            ...source, archive_cleanup_error: 'E'.repeat(4096),
        });
        assert.equal(diagnostics.summary, source.summary);
        assert.equal(diagnostics.archive_cleanup_error.length, 1024);
        assert.equal(diagnostics.display_truncated, true);
        assert.equal(source.summary, compactSummary(bytes));
        assert.ok(Buffer.byteLength(JSON.stringify(diagnostics)) <= 768 * 1024);
    });

    it(`delivers and replays a ${bytes}-byte summary through Hub normalization to panel rounds`, async t => {
        const ctx = await setupPanelHub(t);
        const session = ctx.hub.registry.create('compact-display');
        const worker = await ctx.connect(`/agent/${session.id}/events?token=${session.token}`);
        const { peer } = await ctx.panel();
        await ctx.subscribe(peer, session.id);
        const source = result(compactSummary(bytes));
        worker.send(workerEvent({ session: session.id, worker: 'w', sequence: 1,
            event: 'compact_finished', data: source }));
        const live = await peer.waitFor(message => message.type === 'event'
            && message.envelope.event === 'compact_finished');
        assert.equal(live.envelope.data.summary, source.summary);
        const replay = await ctx.panel();
        const subscribed = await ctx.subscribe(replay.peer, session.id);
        assert.equal(subscribed.transcript.find(event => event.event === 'compact_finished').data.summary, source.summary);
        const store = createPanelStore();
        store.getState().applySubscribed(subscribed);
        const view = store.getState().views.get(session.id);
        const round = buildRounds(view.items, view.confirmations, view.requests)
            .find(round => round.compactResult);
        assert.equal(round.compactResult.summary, source.summary);
    });
}

it('accounts for JSON escaping and explicitly omits oversized or malformed summaries', () => {
    const escaped = result('\x01'.repeat(32768));
    assert.deepEqual(normalizeDisplay('compact_finished', escaped), escaped);
    assert.ok(Buffer.byteLength(JSON.stringify(escaped)) > 190 * 1024);
    const oversized = normalizeDisplay('compact_finished', result('🌍'.repeat(8193)));
    assert.deepEqual(oversized.summary, { display_omitted: true, bytes: 32772,
        reason: 'compact summary exceeds 32768 byte limit' });
    assert.equal(parseCompactResult(oversized), null);
    assert.equal(parseCompactResult(normalizeDisplay('compact_finished', result({ raw: 'invalid' }))), null);
    assert.equal(normalizeDisplay('compact_finished', { durable: false }).durable, false);
    assert.equal(normalizeDisplay('other_event', { summary: 'S'.repeat(8192) }).summary.length, 1024);
});
