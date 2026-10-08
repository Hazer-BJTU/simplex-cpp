import assert from 'node:assert/strict';
import { it } from 'node:test';
import { parseEventEnvelope } from '../src/protocol/events.ts';
import { normalizeDisplay } from '../src/protocol/display.ts';
import { answerPage } from '../shared/answers.ts';
import { RingBuffer } from '../src/util/ring.ts';
import { setupPanelHub } from './helpers/panel.js';
import { workerEvent, until } from './helpers/worker.js';

it('clips reasoning/extras first without mutating a Unicode multipart answer', () => {
    const source = { content: Array.from({ length: 8 }, (_, index) => ({
        type: 'text', modality: 'text', raw: `part ${index}: 中文🌍`,
    })), reasoning: { type: 'text', raw: 'r'.repeat(3 * 1024 * 1024) },
    extras: { duplicate: 'e'.repeat(1024 * 1024) } };
    const copy = structuredClone(source);
    const value = normalizeDisplay('model_response', source);
    assert.deepEqual(value.content, source.content);
    assert.equal(value.reasoning.truncated, true);
    assert.equal(value.reasoning.bytes, 3 * 1024 * 1024);
    assert.equal(value.extras, undefined);
    assert.deepEqual(source, copy);
    assert.ok(Buffer.byteLength(JSON.stringify(value)) < 768 * 1024);
});

it('counts escaped Unicode envelopes and strictly evicts an oversized entry', () => {
    const parsed = parseEventEnvelope(JSON.stringify(workerEvent({ event: 'model_response',
        data: { content: [{ type: 'text', modality: 'text', raw: '中文🌍\n\u0001'.repeat(3000) }] } })));
    assert.equal(parsed.ok, true);
    const wire = JSON.stringify(parsed.envelope);
    assert.ok(Buffer.byteLength(wire) > wire.length);
    const ring = new RingBuffer({ limit: 2, byteLimit: 1024, sizeOf: item => item.bytes });
    ring.push(parsed.envelope);
    assert.equal(ring.size, 0);
    assert.equal(ring.bytes, 0);
});

it('rejects page gaps, stale mixtures, excessive chunks and false completion', () => {
    const query = { source: { worker_id: 'w', turn: 0, step: 0, commit_sequence: '1' }, part: 0, offset: 0 };
    const page = { ...query, request_id: 'r', raw: '中文🌍', type: 'text', modality: 'text',
        bytes: 10, next_offset: 10, next_part: 1, total_parts: 1, done: true };
    assert.ok(answerPage(page, query));
    assert.ok(answerPage({ ...page, source: { ...query.source, fingerprint: 'a'.repeat(64) } },
        { ...query, source: { ...query.source, fingerprint: 'a'.repeat(64) } }));
    for (const value of [{ ...page, source: { ...query.source, fingerprint: 'b'.repeat(64) } }, { ...page, offset: 1 }, { ...page, next_offset: 9 },
        { ...page, done: false }, { ...page, source: { ...query.source, commit_sequence: '2' } },
        { ...page, raw: 'x'.repeat(40000), next_offset: 40000, bytes: 40000 }]) {
        assert.equal(answerPage(value, query), false);
    }
});

it('keeps panels connected and replay advances after an older worker sends 3 MiB reasoning', async t => {
    const ctx = await setupPanelHub(t);
    const session = ctx.hub.registry.create('huge-reasoning');
    const worker = await ctx.connect(`/agent/${session.id}/events?token=${session.token}`);
    worker.send(workerEvent({ session: session.id, worker: 'legacy', sequence: 1,
        event: 'status', data: { active: false } }));
    await until(() => session.connected);
    const first = await ctx.panel();
    await ctx.subscribe(first.peer, session.id);
    worker.send(workerEvent({ session: session.id, worker: 'legacy', sequence: 2,
        event: 'model_response', data: {
            content: [{ type: 'text', modality: 'text', raw: 'exact final answer 中文🌍' }],
            reasoning: { type: 'text', raw: 'r'.repeat(3 * 1024 * 1024) },
        } }));
    const live = await first.peer.waitFor(message => message.type === 'event' && message.envelope.event === 'model_response');
    assert.equal(live.envelope.data.content[0].raw, 'exact final answer 中文🌍');
    assert.equal(live.envelope.raw.data, undefined);
    assert.equal(live.envelope.data.reasoning.truncated, true);
    assert.ok(Buffer.byteLength(JSON.stringify(live)) < 768 * 1024);
    worker.send(workerEvent({ session: session.id, worker: 'legacy', sequence: 3,
        event: 'run_finished', data: { status: 'completed' } }));
    await first.peer.waitFor(message => message.type === 'event' && message.envelope.event === 'run_finished');
    for (let attempt = 0; attempt < 2; ++attempt) {
        const replay = await ctx.panel();
        const response = await ctx.subscribe(replay.peer, session.id);
        assert.equal(response.transcript.find(event => event.event === 'model_response').data.content[0].raw,
            'exact final answer 中文🌍');
        assert.ok(response.transcript.some(event => event.event === 'run_finished'));
    }
});

it('proxies exact answer pages only to the identified live worker and expires requests on disconnect', async t => {
    const ctx = await setupPanelHub(t);
    const session = ctx.hub.registry.create('answer-source');
    const worker = await ctx.connect(`/agent/${session.id}/events?token=${session.token}`);
    worker.send(workerEvent({ session: session.id, worker: 'w', sequence: 1,
        event: 'status', data: { active: false, capabilities: ['answer-pages'] } }));
    await until(() => session.workerCapabilities?.names.includes('answer-pages'));
    const query = { source: { worker_id: 'w', turn: 0, step: 0, commit_sequence: '1' }, part: 0, offset: 0 };
    const request = fetch(`${ctx.base}/api/sessions/${session.id}/answer`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(query) });
    const sent = await worker.waitFor(message => message.type === 'payload' && message.data.operation === 'answer');
    worker.send(workerEvent({ session: session.id, worker: 'w', sequence: 2, event: 'answer', data: {
        ...query, request_id: sent.data.request_id, raw: 'original answer', bytes: 15,
        type: 'text', modality: 'text', next_offset: 15, next_part: 1, total_parts: 1, done: true,
    } }));
    const response = await request;
    assert.equal(response.status, 200);
    assert.equal((await response.json()).raw, 'original answer');
    assert.equal(ctx.hub.transcripts.get(session.id).toArray().some(event => event.event === 'answer'), false);
    const waiting = fetch(`${ctx.base}/api/sessions/${session.id}/answer`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(query) });
    await until(() => worker.messages.filter(message => message.data?.operation === 'answer').length === 2);
    worker.ws.terminate();
    assert.equal((await waiting).status, 409);
});

it('delivers a complete 300 KiB final answer before spending bytes on reasoning', () => {
    const raw = 'a'.repeat(300 * 1024);
    const value = normalizeDisplay('model_response', {
        content: [{ type: 'text', modality: 'text', raw }],
        reasoning: { raw: 'r'.repeat(8 * 1024 * 1024) }, extras: { native: 'x'.repeat(1024 * 1024) },
    });
    assert.equal(value.content[0].raw, raw);
    assert.equal(value.content[0].truncated, undefined);
    assert.equal(value.reasoning.raw.length, 4096);
    assert.equal(value.reasoning.truncated, true);
    assert.equal(value.extras, undefined);
});
