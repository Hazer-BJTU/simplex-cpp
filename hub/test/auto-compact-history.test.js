import assert from 'node:assert/strict';
import { it } from 'node:test';
import { uncoveredInternalHistory } from '../web/src/app/history-rounds.ts';
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
