import assert from 'node:assert/strict';
import { it } from 'node:test';
import { buildRounds } from '../web/src/app/rounds.ts';
import { createRoundProjection } from '../web/src/app/roundProjection.ts';
import { toolCall as call, toolEvent as event, toolResult as result,
    toolTranscript, withToolScanCounts } from './helpers/tool-rounds.js';

const fold = items => buildRounds(items, new Map())[0];

it('folds 1024 identified calls without full-call scans, claimed-key copies or linear membership checks', () => {
    const items = toolTranscript();
    const { value: rounds, counts } = withToolScanCounts(() => buildRounds(items, new Map()));
    assert.deepEqual(counts, { callScanVisits: 0, claimedKeyCopies: 0, membershipVisits: 0 });
    assert.equal(rounds.length, 1);
    const run = rounds[0];
    assert.equal(run.calls.length, 1024);
    assert.equal(run.assistant.length, 32);
    assert.equal(run.timeline.filter(entry => entry.kind === 'calls').length, 0);
    assert.equal(run.timeline.filter(entry => entry.kind === 'tool_omission').length, 32);
    assert.ok(run.timeline.filter(entry => entry.kind === 'tool_omission')
        .every(entry => entry.count === 36 && entry.category === 'calls'));
    assert.equal(run.status, 'completed');
    for (const [index, tool] of run.calls.entries()) {
        const id = `call-${Math.floor(index / 32)}-${index % 32}`;
        assert.equal(tool.id, id);
        assert.equal(tool.result.text, `Answer ${id}`);
        assert.equal(tool.security, 'Trusted');
        assert.equal(tool.status, 'ok');
        assert.equal(tool.elapsedMs, 200);
    }
    const project = createRoundProjection();
    assert.deepEqual(project(items, new Map(), new Map()), rounds);
});

it('preserves first-retained ID correlation for repeated results, duplicate proposals and orphan-first replay', () => {
    const proposal = call('same', 'tool', { original: true });
    const run = fold([event(1, 'model_response', { invokes: [proposal, proposal] }),
        event(2, 'tool_results', [result(proposal, 'first'), result(proposal, 'updated')])]);
    assert.equal(run.calls.length, 1);
    assert.deepEqual(run.assistant[0].callIds, ['same', 'same']);
    assert.equal(run.calls[0].result.text, 'updated');
    assert.equal(run.calls[0].args, proposal.arguments);

    const replay = fold([event(1, 'tool_results', [result(proposal, 'orphan')]),
        event(2, 'model_response', { invokes: [proposal] }),
        event(3, 'tool_results', [result(proposal, 'replacement')])]);
    assert.equal(replay.calls.length, 2);
    assert.equal(replay.calls[0].unmatched, true);
    assert.equal(replay.calls[0].result.text, 'replacement');
    assert.equal(replay.calls[1].result, null);
});

it('handles slot zero and arbitrary map keys without prototype or generated-key collisions', () => {
    const ids = ['__proto__', 'constructor', 'toString', 'orphan-event-1-0'];
    const proposals = ids.map(id => call(id));
    const run = fold([event(1, 'model_response', { invokes: proposals }),
        event(2, 'tool_calls', proposals),
        event(3, 'tool_results', [...proposals].reverse().map(query => result(query, query.id)))]);
    assert.deepEqual(run.calls.map(tool => tool.id), ids);
    assert.deepEqual(run.calls.map(tool => tool.result.text), ids);
    assert.equal({}.polluted, undefined);

    // A malformed ID can equal a later orphan's generated display key. Keep
    // the original key lookup so a mixed dispatch echo still pairs as a whole.
    const collision = call('orphan-event-2-1');
    const legacy = call('', 'legacy', { value: 'paired' });
    const mixed = fold([event(1, 'tool_calls', [collision]),
        event(2, 'tool_results', [result(call('missing'), 'orphan')]),
        event(3, 'model_response', { invokes: [collision, legacy] }),
        event(4, 'tool_calls', [collision, legacy])]);
    assert.equal(mixed.calls.length, 3);
    assert.deepEqual(mixed.assistant[0].callIds, [mixed.calls[0].key, mixed.calls[2].key]);
});

it('checks legacy argument evidence only among unresolved proposals with the same name', () => {
    let unrelatedReads = 0;
    const unrelated = Array.from({ length: 200 }, (_, index) => call('', `other-${index}`,
        new Proxy({ value: index }, { get(target, key) { unrelatedReads += 1; return Reflect.get(target, key); } })));
    const identified = call('known', 'target', { value: 'target' });
    const first = call('', 'target', { value: 'first' });
    const second = call('', 'target', { value: 'second' });
    const run = fold([event(1, 'model_response', { invokes: [...unrelated, identified, first, second] }),
        event(2, 'tool_results', [result(second, 'second'), result(first, 'first'),
            result({ name: 'target' }, 'repeat without proposal')])]);
    assert.equal(unrelatedReads, 0);
    assert.equal(run.calls[200].result, null, 'an identified proposal is never a legacy candidate');
    assert.equal(run.calls[201].result.text, 'first');
    assert.equal(run.calls[202].result.text, 'second');
    assert.equal(run.calls[203].unmatched, true, 'settled legacy calls leave the candidate index');
});

it('retains legacy ambiguity after settlement and allows a fresh proposal after orphan results', () => {
    const same = call('', 'tool', { value: 'same' });
    const unique = call('', 'tool', { value: 'unique' });
    const run = fold([event(1, 'model_response', { invokes: [same, same, unique] }),
        event(2, 'tool_results', [result(unique, 'unique'), result(same, 'ambiguous')]),
        event(3, 'model_response', { invokes: [call('', 'fresh')] }),
        event(4, 'tool_results', [result({ name: 'fresh' }, 'fresh result')])]);
    assert.equal(run.calls[0].result, null);
    assert.equal(run.calls[1].result, null);
    assert.equal(run.calls[2].result.text, 'unique');
    assert.equal(run.calls[3].unmatched, true);
    assert.equal(run.calls[4].result.text, 'fresh result');
});

it('keeps claimed membership across exchanges and settles stable slots after terminal status replacement', () => {
    const identified = call('shared');
    const legacy = call('', 'legacy');
    const run = fold([event(1, 'model_response', { invokes: [identified] }),
        event(2, 'model_response', { invokes: [legacy] }),
        event(3, 'tool_calls', [identified]),
        event(4, 'run_finished', { status: 'failed' }),
        event(5, 'tool_results', [result(identified, 'late ID'), result(legacy, 'late legacy')])]);
    assert.equal(run.calls.length, 2);
    assert.equal(run.timeline.filter(entry => entry.kind === 'calls').length, 0);
    assert.deepEqual(run.calls.map(tool => tool.result.text), ['late ID', 'late legacy']);
});

it('rebuilds indexes through trimming, replay, approval changes and reused IDs in different executions', () => {
    const items = toolTranscript(3, 8);
    const project = createRoundProjection();
    const before = project(items, new Map(), new Map());
    const approval = { confirmation_id: 'approval', settled_at: null, call: call('call-0-0') };
    const pending = project(items, new Map([['approval', approval]]), new Map());
    assert.equal(pending[0].calls[0].status, 'pending');
    const settled = project(items, new Map(), new Map());
    assert.deepEqual(settled, before);
    for (const trim of [1, 2, 3, 4]) {
        const retained = structuredClone(items.slice(trim));
        const actual = project(retained, new Map(), new Map());
        const expected = buildRounds(retained, new Map());
        assert.deepEqual(actual.map(({ key, ...rest }) => rest), expected.map(({ key, ...rest }) => rest));
    }
    const reused = [];
    for (let execution = 0; execution < 3; execution += 1) {
        const extra = { run_id: `run-${execution}`, request_id: `request-${execution}`,
            worker_id: execution === 2 ? 'replacement' : 'worker' };
        reused.push(event(20 + execution * 3, 'tool_calls', [call('shared')], extra),
            event(21 + execution * 3, 'tool_results', [result(call('shared'), `${execution}`)], extra),
            event(22 + execution * 3, 'run_finished', { status: 'completed' }, extra));
    }
    assert.deepEqual(buildRounds(reused, new Map()).map(run => run.calls[0].result.text), ['0', '1', '2']);
});
