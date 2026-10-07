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
        assert.deepEqual(rounds, buildRounds(items, prompts, requests));
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
