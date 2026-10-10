import assert from 'node:assert/strict';
import { it } from 'node:test';
import { buildRounds } from '../web/src/app/rounds.ts';
import { createRoundProjection } from '../web/src/app/roundProjection.ts';

function event(n, name, data) {
    return { kind: 'event', id: `event-${n}`, epoch: 'epoch', envelope: {
        type: 'event', event: name, data, session_id: 'demo', worker_id: 'worker',
        request_id: 'request', run_id: 'run', sequence: n, hub_sequence: n,
        received_at: new Date(1700000000000 + n * 100).toISOString(),
    } };
}

function call(args, id = '') {
    return { id, name: 'run_command', arguments: args };
}

function response(n, calls) {
    return event(n, 'model_response', { invokes: calls });
}

function result(args, text, id = '') {
    return { query: call(args, id), output: { type: 'text', raw: text } };
}

function fold(items) {
    return buildRounds(items, new Map())[0];
}

it('pairs only the next complete legacy dispatch echo, preserving repeated identical calls', () => {
    const proposals = [call({ command: 'echo test' }), call({ command: 'echo test' })];
    const items = [response(1, proposals), event(2, 'tool_calls', structuredClone(proposals))];
    const first = fold(items);
    assert.equal(first.calls.length, 2);
    assert.equal(new Set(first.assistant[0].callIds).size, 2);
    assert.equal(first.timeline.filter(entry => entry.kind === 'calls').length, 0);
    items.push(response(3, structuredClone(proposals)), event(4, 'tool_calls', structuredClone(proposals)));
    const next = fold(items);
    assert.equal(next.calls.length, 4);
    assert.deepEqual(next.calls.slice(0, 2).map(call => call.key), first.calls.map(call => call.key));
    assert.equal(new Set(next.calls.map(call => call.key)).size, 4);
    assert.deepEqual(next.assistant.map(block => block.callIds), [
        next.calls.slice(0, 2).map(call => call.key), next.calls.slice(2).map(call => call.key),
    ]);
    assert.deepEqual(fold(structuredClone(items)), next);
});

it('keeps standalone legacy keys stable when earlier calls are trimmed', () => {
    const items = [event(1, 'tool_calls', [call({ command: 'first' })]),
        event(2, 'tool_calls', [call({ command: 'second' })])];
    const project = createRoundProjection();
    const before = project(items, new Map(), new Map())[0];
    const after = project(items.slice(1), new Map(), new Map())[0];
    assert.equal(after.calls[0].key, before.calls[1].key);
    assert.notEqual(after.calls[0].key, before.calls[0].key);
});

it('uses equal arguments independent of object key order to pair a mixed dispatch batch', () => {
    const run = fold([response(1, [call({ command: 'test', options: { a: 1, b: 2 } }), call({}, 'known')]),
        event(2, 'tool_calls', [call({ options: { b: 2, a: 1 }, command: 'test' }), call({}, 'known')])]);
    assert.equal(run.calls.length, 2);
    assert.deepEqual(run.assistant[0].callIds, run.calls.map(call => call.key));
});

it('does not guess legacy pairing for partial, reordered, changed or already consumed batches', () => {
    const proposals = [call({ command: 'A' }), call({ command: 'B' })];
    for (const dispatch of [proposals.slice(0, 1), [...proposals].reverse(),
        [proposals[0], call({ command: 'changed' })]]) {
        const run = fold([response(1, proposals), event(2, 'tool_calls', dispatch)]);
        assert.equal(run.calls.length, proposals.length + dispatch.length);
        assert.equal(run.timeline.filter(entry => entry.kind === 'calls').length, dispatch.length);
    }
    const run = fold([response(1, proposals), event(2, 'tool_calls', proposals),
        event(3, 'tool_calls', proposals)]);
    assert.equal(run.calls.length, 4);
});

it('attaches out-of-order legacy results only by unique name and argument evidence', () => {
    const run = fold([response(1, [call({ command: 'A' }), call({ command: 'B' })]),
        event(2, 'tool_calls', [call({ command: 'A' }), call({ command: 'B' })]),
        event(3, 'tool_results', [result({ command: 'B' }, 'answer B'), result({ command: 'A' }, 'answer A')])]);
    assert.equal(run.calls.length, 2);
    assert.deepEqual(run.calls.map(call => call.result.text), ['answer A', 'answer B']);
    assert.ok(run.calls.every(call => call.status === 'ok' && !call.unmatched));
});

it('shows ambiguous or mismatched results separately without settling a guessed proposal', () => {
    for (const [calls, args] of [
        [[call({ command: 'A' }), call({ command: 'B' })], undefined],
        [[call({ command: 'same' }), call({ command: 'same' })], { command: 'same' }],
        [[call({ command: 'A' })], { command: 'not A' }],
    ]) {
        const run = fold([response(1, calls), event(2, 'tool_results', [result(args, 'unmatched result')]),
            event(3, 'run_finished', { status: 'completed' })]);
        assert.equal(run.calls.length, calls.length + 1);
        assert.ok(run.calls.slice(0, -1).every(call => call.result === null && call.status === 'unknown'));
        assert.equal(run.calls.at(-1).unmatched, true);
        assert.equal(run.calls.at(-1).result.text, 'unmatched result');
    }
});

it('allows a name-only result for one legacy proposal, never for an ID-bearing proposal', () => {
    const run = fold([response(1, [call({ command: 'A' })]),
        event(2, 'tool_results', [result(undefined, 'legacy answer')])]);
    assert.equal(run.calls.length, 1);
    assert.equal(run.calls[0].result.text, 'legacy answer');
    for (const id of ['', 'wrong-id']) {
        const run = fold([response(1, [call({}, 'known')]),
            event(2, 'tool_results', [result(undefined, 'unmatched answer', id)])]);
        assert.equal(run.calls[0].result, null);
        assert.equal(run.calls[1].unmatched, true);
    }
});

it('supports tool-message result provenance and does not pair dispatch after settlement', () => {
    const proposal = call({ command: 'A' });
    const run = fold([response(1, [proposal]), event(2, 'tool_results', [{
        invoke_return: result({ command: 'A' }, 'answer A'), role: 'tool', type: 'invoke_return',
    }]), event(3, 'tool_calls', [proposal])]);
    assert.equal(run.calls.length, 2);
    assert.equal(run.calls[0].result.text, 'answer A');
    assert.equal(run.calls[1].result, null);
});

it('does not pair legacy dispatch echoes after run completion or across differing omissions', () => {
    const proposals = [call({ command: 'A' })];
    const omitted = { display_omitted: true, omitted_items: 3 };
    const finished = fold([response(1, proposals), event(2, 'run_finished', { status: 'completed' }),
        event(3, 'tool_calls', proposals)]);
    assert.equal(finished.calls.length, 2);
    const differing = fold([response(1, [...proposals, omitted]), event(2, 'tool_calls', proposals)]);
    assert.equal(differing.calls.length, 2);
    const paired = fold([response(1, [...proposals, omitted]), event(2, 'tool_calls', [...proposals, omitted])]);
    assert.equal(paired.calls.length, 1);
    assert.equal(paired.timeline.filter(entry => entry.kind === 'tool_omission').length, 1);
});

it('refuses expensive legacy comparisons without recursion or mutating caller arguments', () => {
    let left = {}, right = {};
    for (let n = 0; n < 10000; n++) {
        left = { child: left };
        right = { child: right };
    }
    const run = fold([response(1, [call(left)]), event(2, 'tool_calls', [call(right)])]);
    assert.equal(run.calls.length, 2);
    assert.equal(run.calls[0].args, left);
    assert.equal(run.calls[1].args, right);
});
