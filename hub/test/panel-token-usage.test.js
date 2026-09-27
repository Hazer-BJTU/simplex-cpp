import assert from 'node:assert/strict';
import { it } from 'node:test';
import { formatTokens, formatCacheRate, parseTokenUsage, tokenUsageBand } from '../web/src/state/tokenUsage.ts';
import { emptyView, indexEnvelope } from '../web/src/state/view.ts';

it('formats decimal token units with one fractional digit', () => {
    for (const [value, text] of [[0, '0.0'], [64, '64.0'], [4895, '4.9K'],
        [1000, '1.0K'], [1234567, '1.2M'], [1000000000, '1.0B']]) {
        assert.equal(formatTokens(value), text);
    }
    assert.equal(formatCacheRate({ prompt: 1000, generated: 20, cacheHit: 625 }), '62.5%');
    assert.equal(formatCacheRate({ prompt: 0, generated: 0, cacheHit: 0 }), '0.0%');
    assert.equal(parseTokenUsage({ prompt: -1 }), null);
    assert.equal(parseTokenUsage({}), null);
});

it('keeps only the latest response cost and ignores absent costs and stale replay', () => {
    const response = (sequence, cost) => ({ event: 'model_response',
        worker_id: 'worker', sequence, data: { cost } });
    let view = indexEnvelope(emptyView('demo'), response(1,
        { prompt: 1000, generated: 20, cache_hit: 500 }));
    view = indexEnvelope(view, response(2, undefined));
    assert.equal(view.tokenUsage.prompt, 1000);
    view = indexEnvelope(view, response(3, { prompt: 10, generated: 0, cache_hit: 0 }));
    view = indexEnvelope(view, response(1, { prompt: 1000, generated: 20, cache_hit: 500 }));
    assert.deepEqual(view.tokenUsage,
        { prompt: 10, generated: 0, cacheHit: 0, workerId: 'worker', sequence: 3 });
    view = indexEnvelope(view, { ...response(1, { prompt: 0, generated: 0, cache_hit: 0 }),
        worker_id: 'replacement' });
    assert.equal(view.tokenUsage.prompt, 0);
});


it('uses eight binary 128K bands and saturates at 1M without double-counting cache hits', () => {
    const measure = (prompt, generated = 0) => tokenUsageBand({ prompt, generated, cacheHit: prompt });
    assert.equal(measure(0).level, 1);
    assert.deepEqual(measure(0).fills, Array(8).fill(0));
    assert.equal(measure(128 * 1024).level, 1);
    assert.equal(measure(128 * 1024, 1).level, 2);
    assert.equal(measure(64 * 1024).fills[0], 0.5);
    assert.equal(measure(1024 * 1024).level, 8);
    assert.equal(measure(1024 * 1024).upper, '1M');
    assert.deepEqual(measure(2 * 1024 * 1024).fills, measure(1024 * 1024).fills);
    assert.equal(measure(2 * 1024 * 1024).capped, 1024 * 1024);
});
