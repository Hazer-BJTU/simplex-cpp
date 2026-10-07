import assert from 'node:assert/strict';
import { it } from 'node:test';
import { reconcileInternalHistory, uncoveredInternalHistory } from '../web/src/app/history-rounds.ts';
import { buildRounds } from '../web/src/app/rounds.ts';
import { createPanelStore } from '../web/src/state/store.ts';
import { parseHistoryPage } from '../web/src/state/history.ts';
const source = { worker_id: 'worker', request_id: 'task', run_id: 'run' };
const turn = { index: 0, internal_input: 'auto_compact_continue', source, user: [],
    steps: [3, 4].map(index => ({ index: index - 3, commit_sequence: String(index),
        content: [{ type: 'text', modality: 'text', raw: `answer ${index}` }], tool_calls: 0 })), omitted_steps: 0 };
const response = (sequence, commit, ids = source) => ({ envelope: { ...ids, sequence,
    event: 'model_response', data: { commit_sequence: commit } } });
it('correlates internal answers across refresh/reload, partial replay and worker incarnations', () => {
    assert.deepEqual(uncoveredInternalHistory([turn], [], 10), [turn]);
    const runs = [{ assistant: [response(1, '1'), response(5, '3'), response(6, '4')] }];
    assert.deepEqual(uncoveredInternalHistory([turn], runs, 10), []);
    assert.deepEqual(uncoveredInternalHistory([turn], runs, 5)[0].steps, [turn.steps[1]]);
    const other = [{ assistant: [response(5, '3', { ...source, worker_id: 'other' })] }];
    assert.deepEqual(uncoveredInternalHistory([turn], other, 10), [turn]);
});
it('requires host correlation and empty user projection for an internal history turn', () => {
    const page = { request_id: 'query', revision: 5, start: 0, step: 0, next: 1, next_step: 0,
        total: 1, turns: [turn] };
    assert.ok(parseHistoryPage(page));
    assert.equal(parseHistoryPage({ ...page, turns: [{ ...turn, source: undefined }] }), null);
    assert.equal(parseHistoryPage({ ...page, turns: [{ ...turn, user: turn.steps[0].content }] }), null);
});

const session = { session_id: 'demo', spec: {}, connected: true, confirmations: [], requests: [],
    identity: { state: 'live', worker_id: 'worker', since: null }, stats: {}, process: null };
const part = raw => ({ type: 'text', modality: 'text', raw });
function event(sequence, name, execution, data, hubSequence = sequence) {
    return { type: 'event', event: name, session_id: 'demo', ...execution, sequence,
        hub_sequence: hubSequence, data, received_at: '2026-10-07T00:00:00Z' };
}
function executionTranscript(status, restarted) {
    const second = { worker_id: restarted ? 'restarted' : 'worker', request_id: 'continue', run_id: 'resume' };
    let sequence = 0;
    let hubSequence = 0;
    const events = [];
    const add = (name, ids, data) => events.push(event(++sequence, name, ids, data, ++hubSequence));
    const query = { id: 'call-A', name: 'run_command', arguments: { command: 'work' } };
    add('input_admitted', source, { operation: 'message' });
    add('run_started', source, {});
    add('input_committed', source, {});
    add('compact_finished', source, { origin: 'automatic', summary: 'handoff',
        memory_file: '/memory/1/state.md', revision: 3, removed_turns: 1, durable: true });
    add('model_response', source, { commit_sequence: '3', content: [part('Answer A')], invokes: [query] });
    add('tool_calls', source, [query]);
    add('tool_results', source, [{ invoke_return: { query, output: part('Output A') } }]);
    add('run_finished', source, { status });
    if (restarted) sequence = 0;
    add('input_admitted', second, { operation: 'continue' });
    add('run_started', second, {});
    add('model_response', second, { commit_sequence: '4', content: [part('Answer B')] });
    add('run_finished', second, { status: 'completed' });
    const history = { ...turn, steps: [
        { index: 0, commit_sequence: '3', execution: source, content: [part('Answer A')], tool_calls: 1 },
        { index: 1, commit_sequence: '4', execution: second, content: [part('Answer B')], tool_calls: 0 },
    ] };
    return { events, history, second, baseline: sequence + 1 };
}

for (const status of ['cancelled', 'failed']) {
    for (const restarted of [false, true]) {
        for (const missing of [null, 'A', 'B']) {
            it(`deduplicates ${status} → Continue, restart=${restarted}, missing=${missing}, on refresh/reload`, () => {
                const fixture = executionTranscript(status, restarted);
                const replay = fixture.events.filter(item => !(item.event === 'model_response'
                    && item.request_id === (missing === 'A' ? 'task' : missing === 'B' ? 'continue' : '')));
                for (const reloaded of [false, true]) {
                    const store = createPanelStore();
                    if (reloaded) store.getState().applySubscribed({ type: 'subscribed', session,
                        transcript: replay, logs: [], latest: replay.at(-1).hub_sequence });
                    else for (const envelope of replay) store.getState().applyEvent({ type: 'event',
                        session: 'demo', hub_seq: envelope.hub_sequence, envelope });
                    store.getState().beginHistory('demo');
                    const page = { request_id: 'inspect', revision: 10, start: 0, step: 0,
                        next: 1, next_step: 0, total: 1, turns: [fixture.history] };
                    assert.ok(parseHistoryPage(page));
                    assert.equal(store.getState().applyHistoryPage('demo',
                        event(fixture.baseline, 'history', fixture.second, page), page), true);
                    const view = store.getState().views.get('demo');
                    const runs = buildRounds(view.items, view.confirmations, view.requests)
                        .filter(round => round.kind === 'run');
                    const restored = reconcileInternalHistory(view.history, runs, view.historySequence, view.historyWorker);
                    assert.deepEqual(restored.history, []);
                    assert.deepEqual(restored.rounds.map(round => round.assistant.map(block => block.text)),
                        [['Answer A'], ['Answer B']]);
                    assert.deepEqual(restored.rounds.map(round => round.assistant.length), [1, 1]);
                    assert.equal(restored.rounds[0].calls[0].id, 'call-A');
                    assert.equal(restored.rounds[0].calls[0].result.text, 'Output A');
                    assert.equal(restored.rounds[1].calls.length, 0);
                    assert.equal(restored.rounds[1].continued, true);
                    assert.equal(restored.rounds[1].input, null);
                    assert.deepEqual(view.history[0].user, []);
                    assert.deepEqual(view.history[0].source, source);
                    for (const round of restored.rounds) {
                        const entries = round.timeline.filter(item => item.kind === 'assistant');
                        assert.equal(entries.length, 1);
                        const end = round.timeline.findIndex(item => item.kind === 'protocol'
                            && round.protocol.some(event => event.id === item.key && event.envelope.event === 'run_finished'));
                        assert.ok(round.timeline.indexOf(entries[0]) < end);
                    }
                }
            });
        }
    }
}

it('orders recovered responses among live responses while preserving tool cards', () => {
    const fixture = executionTranscript('failed', false);
    const extra = { index: 2, commit_sequence: '5', execution: fixture.second,
        content: [part('Answer C')], tool_calls: 0 };
    fixture.history.steps.push(extra);
    const events = fixture.events.filter(item => !(item.event === 'model_response' && item.request_id === 'continue'));
    events.splice(-1, 0, event(12, 'model_response', fixture.second,
        { commit_sequence: '5', content: extra.content }));
    const store = createPanelStore();
    store.getState().applySubscribed({ type: 'subscribed', session, transcript: events, logs: [], latest: 13 });
    const view = store.getState().views.get('demo');
    const runs = buildRounds(view.items, view.confirmations, view.requests).filter(round => round.kind === 'run');
    const result = reconcileInternalHistory([fixture.history], runs, 20, 'worker');
    assert.deepEqual(result.rounds[1].assistant.map(block => block.text), ['Answer B', 'Answer C']);
    assert.deepEqual(result.rounds[1].timeline.filter(entry => entry.kind === 'assistant').map(entry => entry.key),
        result.rounds[1].assistant.map(block => block.key));
});

it('does not deduplicate unrelated execution identities with the same commit sequence', () => {
    const fixture = executionTranscript('failed', true);
    const unrelated = { ...fixture.second, run_id: 'divergent-run' };
    const runs = [{ assistant: [response(2, '4', unrelated)] }];
    assert.deepEqual(uncoveredInternalHistory([fixture.history], runs, 10, fixture.second.worker_id), [fixture.history]);
    const page = { request_id: 'inspect', revision: 10, start: 0, step: 0,
        next: 1, next_step: 0, total: 1, turns: [fixture.history] };
    const malformed = structuredClone(page);
    malformed.turns[0].steps[1].execution.run_id = '';
    assert.equal(parseHistoryPage(malformed), null);
});

it('keeps unmatched executions as history and ignores malformed replay commit identifiers', () => {
    const fixture = executionTranscript('failed', false);
    const store = createPanelStore();
    store.getState().applySubscribed({ type: 'subscribed', session, transcript: fixture.events,
        logs: [], latest: fixture.events.at(-1).hub_sequence });
    const view = store.getState().views.get('demo');
    const runs = buildRounds(view.items, view.confirmations, view.requests).filter(round => round.kind === 'run');
    const divergent = { ...fixture.history, steps: [{ ...fixture.history.steps[1],
        execution: { ...fixture.second, run_id: 'unrelated-run' } }] };
    const unmatched = reconcileInternalHistory([divergent], runs, 20, 'worker');
    assert.deepEqual(unmatched.history, [divergent]);
    assert.deepEqual(unmatched.rounds, runs);
    const malformed = runs.map(round => ({ ...round, assistant: round.assistant.map(block =>
        ({ ...block, envelope: { ...block.envelope, data: { commit_sequence: 'not-an-integer' } } })) }));
    const restored = reconcileInternalHistory([fixture.history], malformed, 20, 'worker');
    assert.equal(restored.history.length, 0);
    assert.deepEqual(restored.rounds.map(round => round.assistant.at(-1).text), ['Answer A', 'Answer B']);
});
