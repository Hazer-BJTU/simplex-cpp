import assert from 'node:assert/strict';
import { test } from 'node:test';
import { receivePreview } from '../src/subagents/receive-preview.ts';

const part = raw => ({ type: 'text', modality: 'text', raw });
const conversation = steps => ({ revision: 1, worker_id: 'child', stale: false,
    incomplete: false, truncated: false, refreshed_at: null,
    turns: [{ index: 0, request_id: 'request', user: [part('task')], steps }] });

test('receive preview favors the latest visible reply and hides transport identities', () => {
    const result = receivePreview(conversation([
        { index: 0, content: [part('earlier')] },
        { index: 1, content: [part('final')], execution: { worker_id: 'worker', request_id: 'response-request', run_id: 'run' }, answer_source: { fingerprint: 'secret' } },
        { index: 2, content: [] },
    ]));
    assert.equal(result.response, 'final');
    assert.equal(result.request_id, 'response-request');
    assert.equal(result.truncated, false);
    assert.ok(!JSON.stringify(result).includes('fingerprint'));
});

test('receive preview clips Unicode and preserves source completeness flags', () => {
    const source = conversation([{ index: 0, content: [part('🌻'.repeat(10000))] }]);
    const result = receivePreview(source);
    assert.ok(Buffer.byteLength(result.response) <= 16 * 1024);
    assert.ok(!result.response.includes('�'));
    assert.equal(result.truncated, true);
    source.turns[0].steps[0].content = [{ ...part('prefix'), truncated: true }];
    assert.equal(receivePreview(source).truncated, true);
    assert.equal(receivePreview(null).incomplete, true);
});

test('a finished compact summary remains visible after turns are removed', () => {
    const result = receivePreview(null, { request_id: 'compact-request', summary: 'saved summary' });
    assert.equal(result.response, 'saved summary');
    assert.equal(result.request_id, 'compact-request');
});

test('unknown response provenance is never inferred from the turn request', () => {
    const result = receivePreview(conversation([{ index: 0, content: [part('legacy response')] }]));
    assert.equal(result.response, 'legacy response');
    assert.ok(!('request_id' in result));
});
