import assert from 'node:assert/strict';
import { it } from 'node:test';
import { buildRounds } from '../web/src/app/rounds.ts';
import { reconcileHistory } from '../web/src/app/history-rounds.ts';
import { createPanelStore } from '../web/src/state/store.ts';
import { parseHistoryPage } from '../web/src/state/history.ts';
import { indexEnvelope, viewDisplayBytes } from '../web/src/state/view.ts';

const part = raw => ({ type: 'text', modality: 'text', raw });
const execution = (request, worker = 'worker') => ({ worker_id: worker, request_id: request, run_id: `run-${request}` });
const A = execution('A'), B = execution('B');
function event(sequence, name, source, data = {}) {
    return { kind: 'event', id: `${source.worker_id}-${sequence}`, epoch: 'epoch', envelope: {
        type: 'event', event: name, session_id: 'demo', ...source, sequence, hub_sequence: sequence, data,
    } };
}
function runEvents(source, text, commit, start = 1, operation = 'message') {
    return [event(start, 'input_admitted', source, { operation }),
        event(start + 1, 'input_committed', source),
        event(start + 2, 'model_response', source, { content: [part(text)], commit_sequence: commit }),
        event(start + 3, 'run_finished', source, { status: 'completed' })];
}
function turn(index, source, text, steps = []) {
    return { index, source, user: [part(text)], steps, omitted_steps: 0 };
}
function step(index, source, text, commit) {
    return { index, execution: source, commit_sequence: commit, content: [part(text)], tool_calls: 0 };
}
function reconcile(history, items, baseline = 100, worker = 'worker') {
    const runs = buildRounds(items, new Map()).filter(round => round.kind === 'run');
    return reconcileHistory(history, runs, baseline, worker);
}

it('does not shift old history onto a newer admitted input during concurrent history refresh', () => {
    const history = [turn(0, A, 'Input A', [step(0, A, 'Answer A', '1')])];
    const items = [...runEvents(A, 'Answer A', '1'), event(5, 'input_admitted', B, { operation: 'message' }),
        event(6, 'input_committed', B)];
    const result = reconcile(history, items);
    assert.deepEqual([...result.historyForRun.values()].map(turn => turn.user[0].raw), ['Input A']);
    assert.equal(result.historyForRun.has(result.restoredRuns[1].key), false);
    assert.deepEqual(result.olderHistory, []);
    for (const replay of [items, structuredClone(items), items.slice(2)]) {
        const result = reconcile(history, replay);
        assert.equal(result.historyForRun.get(result.restoredRuns[0].key).source.request_id, 'A');
        assert.equal(result.historyForRun.has(result.restoredRuns[1].key), false);
    }
});

it('restores each Continue response to its execution without repeating the original user input', () => {
    const history = [turn(0, A, 'Original input', [step(0, A, 'Answer A', '1'), step(1, B, 'Answer B', '2')])];
    const items = [...runEvents(A, 'Answer A', '1'),
        event(5, 'input_admitted', B, { operation: 'continue' }), event(6, 'run_finished', B, { status: 'completed' })];
    const result = reconcile(history, items);
    assert.deepEqual(result.restoredRuns.map(round => round.assistant.map(block => block.text)), [['Answer A'], ['Answer B']]);
    assert.equal(result.historyForRun.size, 1);
    assert.equal(result.historyForRun.has(result.restoredRuns[1].key), false);
    assert.deepEqual(result.olderHistory, []);
});

it('matches unanswered and empty inputs by source without needing a model response', () => {
    const history = [turn(0, A, '')];
    const result = reconcile(history, [event(1, 'input_admitted', A, { operation: 'message' }),
        event(2, 'input_committed', A), event(3, 'run_finished', A, { status: 'cancelled' })]);
    assert.equal(result.historyForRun.get(result.restoredRuns[0].key).user[0].raw, '');
    assert.deepEqual(result.olderHistory, []);
});

it('keeps legacy input separate even when a response identity can be restored', () => {
    const history = [turn(0, undefined, 'Legacy input', [step(0, A, 'Answer A', '1')])];
    const result = reconcile(history, [event(1, 'run_finished', A, { status: 'completed' })]);
    assert.equal(result.historyForRun.size, 0);
    assert.equal(result.olderHistory[0].user[0].raw, 'Legacy input');
    assert.deepEqual(result.olderHistory[0].steps, []);
    assert.equal(result.restoredRuns[0].assistant[0].text, 'Answer A');
    const noMetadata = [{ ...history[0], steps: [{ ...history[0].steps[0], execution: undefined }] }];
    assert.deepEqual(reconcile(noMetadata, runEvents(A, 'Answer A', '1')).olderHistory, noMetadata);
});

it('separates worker incarnations, future replay, duplicate sources and duplicate execution owners', () => {
    const history = [turn(0, A, 'Input A', [step(0, A, 'Answer A', '1')])];
    for (const items of [runEvents(execution('A', 'other'), 'Other answer', '1'),
        runEvents(A, 'Future answer', '1', 101),
        [...runEvents(A, 'First answer', '1'), ...runEvents(A, 'Second answer', '2', 5)]]) {
        const result = reconcile(history, items);
        assert.equal(result.historyForRun.size, 0);
        assert.equal(result.olderHistory[0].user[0].raw, 'Input A');
    }
    const duplicates = reconcile([...history, turn(1, A, 'Different input')], runEvents(A, 'Answer A', '1'));
    assert.equal(duplicates.historyForRun.size, 0);
    const old = reconcile(history, runEvents(A, 'Answer A', '1', 101), 1, 'restarted-worker');
    assert.equal(old.historyForRun.size, 1, 'the new incarnation cursor must not bound old executions');
});

it('keeps compact continuation input private and does not resurrect the removed ordinary input', () => {
    const internal = { ...turn(0, A, ''), user: [], internal_input: 'auto_compact_continue',
        steps: [step(0, A, 'After compact', '2')] };
    const result = reconcile([internal], [...runEvents(A, 'Before compact', '1'),
        event(5, 'model_response', A, { commit_sequence: '2', content: [part('After compact')] })]);
    assert.equal(result.historyForRun.size, 0);
    assert.deepEqual(result.olderHistory, []);
    assert.deepEqual(result.restoredRuns[0].assistant.map(block => block.text), ['Before compact', 'After compact']);
});

function page(start, total, turns, overrides = {}) {
    return { request_id: `history-${start}`, revision: 1, start, step: 0,
        next: start + turns.length, next_step: 0, total, turns, ...overrides };
}
function apply(store, data, sequence, worker = 'worker') {
    const parsed = parseHistoryPage(data);
    assert.ok(parsed);
    return store.getState().applyHistoryPage('demo', event(sequence, 'history', execution('history', worker), data).envelope, parsed);
}

it('publishes a validated refresh atomically and preserves the previous snapshot on failure', () => {
    const store = createPanelStore();
    const old = turn(0, A, 'Previous input');
    assert.equal(apply(store, page(0, 1, [old]), 1), true);
    for (const invalid of [{ revision: 2 }, { total: 3 }]) {
        store.getState().beginHistory('demo');
        assert.equal(apply(store, page(0, 2, [turn(0, A, 'Replacement input')]), 2), true);
        assert.deepEqual(store.getState().view('demo').history, [old]);
        assert.equal(apply(store, page(1, 2, [turn(1, B, 'Input B')], invalid), 3), false);
        store.getState().endHistory('demo');
        assert.deepEqual(store.getState().view('demo').history, [old]);
        assert.equal(store.getState().view('demo').historyLoad, null);
    }
    store.getState().beginHistory('demo');
    apply(store, page(0, 2, [turn(0, A, 'New A')]), 4);
    assert.equal(apply(store, page(1, 2, [turn(1, B, 'New B')]), 5), true);
    assert.deepEqual(store.getState().view('demo').history.map(turn => turn.user[0].raw), ['New A', 'New B']);
    assert.equal(store.getState().view('demo').historyRevision, 1);
});

it('rejects changed source/user, worker or sequence while a turn is fragmented', () => {
    const first = page(0, 1, [{ ...turn(0, A, 'Input'), steps: [step(0, A, 'A', '1')], omitted_steps: 1 }],
        { next: 0, next_step: 1 });
    const next = page(0, 1, [turn(0, A, 'Input', [step(1, A, 'B', '2')])], { step: 1 });
    for (const [incoming, sequence, worker] of [
        [next, 3, 'other'], [next, 2, 'worker'],
        [{ ...next, turns: [{ ...next.turns[0], source: B }] }, 3, 'worker'],
        [{ ...next, turns: [{ ...next.turns[0], user: [part('Changed')] }] }, 3, 'worker'],
    ]) {
        const store = createPanelStore();
        apply(store, first, 2);
        assert.equal(apply(store, incoming, sequence, worker), false);
        assert.deepEqual(store.getState().view('demo').history, []);
    }
});

it('validates ordinary input source metadata without requiring it from older snapshots', () => {
    const valid = page(0, 1, [turn(0, A, 'Input')]);
    assert.ok(parseHistoryPage(valid));
    assert.ok(parseHistoryPage({ ...valid, turns: [turn(0, undefined, 'Legacy')] }));
    for (const source of [null, [], {}, { ...A, worker_id: '' }]) {
        assert.equal(parseHistoryPage({ ...valid, turns: [turn(0, source, 'Invalid')] }), null);
    }
});

it('counts unpublished pages in the display budget and invalidates them at compact', () => {
    const store = createPanelStore();
    const first = page(0, 1, [{ ...turn(0, A, 'Unpublished input'),
        steps: [step(0, A, 'Unpublished answer', '1')], omitted_steps: 1 }],
    { next: 0, next_step: 1 });
    apply(store, first, 2);
    const view = store.getState().view('demo');
    assert.ok(viewDisplayBytes(view) > viewDisplayBytes({ ...view, historyLoad: null }));
    const compact = event(3, 'compact_finished', A, { summary: 'handoff', memory_file: '/memory/1.md',
        removed_turns: 1, revision: 2, durable: true }).envelope;
    const cleared = indexEnvelope(view, compact);
    assert.equal(cleared.historyLoad, null);
    assert.equal(cleared.historyLoading, false);
    assert.deepEqual(cleared.history, []);
});
