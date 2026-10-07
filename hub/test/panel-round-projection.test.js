import assert from 'node:assert/strict';
import { it } from 'node:test';
import { buildRounds } from '../web/src/app/rounds.ts';
import { createRoundProjection } from '../web/src/app/roundProjection.ts';

function event(n, name, request, data, worker = 'worker') {
    return { kind: 'event', id: `${worker}-${n}`, epoch: 'epoch', envelope: {
        event: name, data, type: 'event', session_id: 'demo', worker_id: worker,
        request_id: request, run_id: `run-${request}`, sequence: n, hub_sequence: n,
        received_at: new Date(1700000000000 + n * 100).toISOString(),
    } };
}

it('matches the reference through append, late approval, request updates, replay, trim and worker replacement', () => {
    const project = createRoundProjection();
    let items = [], prompts = new Map(), requests = new Map();
    function verify() {
        const rounds = project(items, prompts, requests);
        // Projection owns DOM identity; all other data matches the reference.
        const withoutKeys = values => values.map(({ key, ...round }) => round);
        assert.deepEqual(withoutKeys(rounds), withoutKeys(buildRounds(items, prompts, requests)));
        return rounds;
    }
    // Cross-panel admission precedes our local outbox's execution.
    items.push({ kind: 'outbox', id: 'B', requestId: 'B', operation: 'message',
        parts: [{ type: 'text', raw: 'B' }], state: 'sent', sentAt: '2023-11-14T22:13:20Z' });
    for (let index = 0; index < 20; index++) {
        const id = index === 0 ? 'A' : index === 1 ? 'B' : `request-${index}`;
        const before = verify();
        const steps = [
            ['input_admitted', { operation: 'message' }],
            ['model_response', { content: [{ type: 'text', raw: `Answer ${id}` }] }],
            ['tool_calls', [{ id, name: 'run_command', arguments: { command: `echo ${id}` } }]],
            ['tool_results', [{ query: { id, name: 'run_command' }, output: { type: 'text', raw: '[[state]]: exited\n\nstdout (2 bytes):\nok' } }]],
            ['run_finished', { status: 'completed', exchanges: 1 }],
        ];
        for (const [offset, [name, data]] of steps.entries()) {
            items = [...items, event(index * 6 + offset, name, id, data)]; verify();
        }
        const after = verify();
        for (const previous of before.filter(round => round.status === 'completed')) {
            assert.equal(after.find(round => round.key === previous.key), previous);
        }
    }
    const before = verify();
    prompts = new Map([['p', { confirmation_id: 'p', settled_at: null,
        call: { id: 'request-19', name: 'run_command' } }]]);
    const pending = verify();
    assert.equal(pending[0], before[0]);
    assert.equal(pending.at(-1).calls[0].status, 'pending');
    prompts = new Map(); verify();
    requests = new Map([['B', { request_id: 'B', operation: 'continue' }]]); verify();
    items = structuredClone(items); verify();
    items = items.slice(31); verify();
    items.push(event(200, 'compact_finished', 'compact', { summary: 'saved', memory_file: '/memory/a.md', removed_turns: 20 })); verify();
    items = [event(1, 'input_admitted', 'A', { operation: 'continue' }, 'replacement')]; verify();
    items = []; prompts = new Map(); requests = new Map(); assert.deepEqual(verify(), []);
});

it('does not traverse arbitrary wire JSON or mutate caller-owned arguments', () => {
    const project = createRoundProjection();
    let args = { value: 'leaf' };
    for (let n = 0; n < 10000; n++) args = { child: args };
    const items = [event(1, 'tool_calls', 'A', [{ id: 'call', name: 'tool', arguments: args }])];
    const first = project(items, new Map(), new Map());
    assert.equal(project([...items], new Map(), new Map())[0], first[0]);
    assert.equal(first[0].calls[0].args, args);
    const dangerous = JSON.parse('{"__proto__":{"x":1}}');
    items.push(event(2, 'tool_calls', 'A', [{ id: 'new', name: 'tool', arguments: dangerous }]));
    assert.equal(project(items, new Map(), new Map())[0].calls[1].args, dangerous);
    assert.equal({}.x, undefined);
});

it('retention does not transfer a surviving execution to another round DOM key', () => {
    const project = createRoundProjection();
    const items = [];
    for (let n = 0; n < 6; n++) {
        items.push(event(n * 3 + 1, 'input_admitted', `request-${n}`, { operation: 'message' }),
            event(n * 3 + 2, 'tool_calls', `request-${n}`, [{ id: `call-${n}`, name: 'tool', arguments: {} }]),
            event(n * 3 + 3, 'run_finished', `request-${n}`, { status: 'completed' }));
    }
    const before = project(items, new Map(), new Map());
    const after = project(items.slice(6), new Map(), new Map());
    for (const round of after) {
        const original = before.find(value => value.calls[0].id === round.calls[0].id);
        assert.equal(round.key, original.key);
        assert.equal(round.calls, original.calls);
    }
    assert.equal(after[0].index, 1); // The visible ordinal still reflects retained order.
});

it('older uncorrelated executions have distinct presentation keys', () => {
    const project = createRoundProjection();
    const items = [];
    for (let n = 0; n < 2; n++) {
        const call = event(n * 2 + 1, 'tool_calls', '', [{ id: `legacy-${n}`, name: 'tool', arguments: {} }]);
        const finished = event(n * 2 + 2, 'run_finished', '', { status: 'completed' });
        call.envelope.run_id = finished.envelope.run_id = '';
        items.push(call, finished);
    }
    const before = project(items, new Map(), new Map());
    assert.equal(before.length, 2);
    assert.equal(new Set(before.map(round => round.key)).size, 2);
    const after = project(items.slice(2), new Map(), new Map());
    assert.equal(after[0].key, before[1].key);
    assert.equal(after[0].calls, before[1].calls);
});

for (const operation of ['message', 'continue', 'compact']) {
    it(`preserves ${operation} identity when its outbox/admission and part of its response are trimmed`, () => {
        const project = createRoundProjection();
        const outbox = { kind: 'outbox', id: 'local', requestId: 'request', operation,
            parts: [{ type: 'text', raw: 'input' }], state: 'admitted', admittedWorker: 'worker', admittedSequence: 1 };
        let items = [outbox, event(1, 'input_admitted', 'request', { operation }),
            event(2, 'model_response', 'request', { content: [{ type: 'text', raw: 'Earlier response' }] }),
            event(3, 'tool_calls', 'request', [{ id: 'call', name: 'tool', arguments: {} }]),
            event(4, 'model_response', 'request', { content: [{ type: 'text', raw: 'Surviving response' }] })];
        const original = project(items, new Map(), new Map())[0];
        for (let trim = 0; trim < 3; trim++) {
            items = items.slice(1);
            const current = project(items, new Map(), new Map())[0];
            assert.equal(current.key, original.key);
            assert.equal(current.calls, original.calls);
            assert.deepEqual(current.sourceKeys, buildRounds(items, new Map())[0].sourceKeys);
        }
        // A retained assistant response alone still preserves the round key.
        items = items.slice(1);
        assert.equal(project(items, new Map(), new Map())[0].key, original.key);
    });
}

it('distinguishes reused wire IDs through partial retention and after all old evidence is replaced', () => {
    const project = createRoundProjection();
    const first = [event(1, 'input_admitted', 'same', { operation: 'message' }),
        event(2, 'model_response', 'same', { content: [{ type: 'text', raw: 'First' }] }),
        event(3, 'run_finished', 'same', { status: 'completed' })];
    const second = [event(4, 'input_admitted', 'same', { operation: 'message' }),
        event(5, 'model_response', 'same', { content: [{ type: 'text', raw: 'Second' }] })];
    const before = project([...first, ...second], new Map(), new Map());
    assert.equal(before.length, 2);
    assert.notEqual(before[0].key, before[1].key);
    const after = project(second.slice(1), new Map(), new Map());
    assert.equal(after[0].key, before[1].key);
    // Shared wire IDs with no retained source do not grant DOM continuity.
    const replacement = project([event(6, 'model_response', 'same', {
        content: [{ type: 'text', raw: 'New fragment' }],
    })], new Map(), new Map());
    assert.notEqual(replacement[0].key, after[0].key);
    const newEpoch = [{ ...second[1], epoch: 'replacement-epoch' }];
    assert.notEqual(project(newEpoch, new Map(), new Map())[0].key, replacement[0].key);
    assert.deepEqual(project([], new Map(), new Map()), []);
});
