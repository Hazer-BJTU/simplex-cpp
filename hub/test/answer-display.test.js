import assert from 'node:assert/strict';
import { it } from 'node:test';
import { parseEventEnvelope } from '../src/protocol/events.ts';
import { normalizeDisplay } from '../src/protocol/display.ts';
import { answerPage } from '../shared/answers.ts';
import { RingBuffer } from '../src/util/ring.ts';
import { setupPanelHub } from './helpers/panel.js';
import { workerEvent, until } from './helpers/worker.js';
import { buildRounds } from '../web/src/app/rounds.ts';

function structuredArguments() {
    return { nodes: Array.from({ length: 64 }, (_, index) => ({ first: index, second: index })) };
}

it('preserves tool identity, independent batch entries and protected result annotations', () => {
    const calls = Array.from({ length: 3 }, (_, index) => ({
        arguments: structuredArguments(), extras: { metadata: structuredArguments() },
        id: `call-${index}`, name: `structured_tool_${index}`, security: 'require_confirm', type: 'serial_write',
    }));
    assert.ok(Buffer.byteLength(JSON.stringify(calls[0])) < 4096);
    const originalCalls = structuredClone(calls);
    const proposals = normalizeDisplay('tool_calls', calls);
    const model = normalizeDisplay('model_response', { content: [], invokes: calls });
    assert.deepEqual(model.invokes, proposals);
    const results = calls.map(query => ({
        // Metadata appears first in the old generic traversal; neither it nor
        // the output body may erase the subsequent query and outcome fields.
        extras: { aaa_metadata: structuredArguments(), error: { stage: 'invoke', message: 'distinct error' },
            status: 'failed', loop_skipped: true },
        output: { extras: { metadata: structuredArguments() }, raw: 'output 中文🌍', type: 'text', modality: 'text' },
        query,
    }));
    const messages = results.map(record => ({ content: [record.output], invoke_return: record, role: 'tool', type: 'invoke_return' }));
    const originals = structuredClone({ results, messages });
    for (const input of [results, messages]) {
        const projected = normalizeDisplay('tool_results', input);
        assert.equal(projected.length, calls.length);
        for (let index = 0; index < calls.length; index++) {
            const returned = projected[index].invoke_return ?? projected[index];
            for (const key of ['id', 'name', 'security', 'type']) {
                assert.equal(proposals[index][key], calls[index][key]);
                assert.equal(returned.query[key], calls[index][key]);
            }
            assert.equal(returned.output.raw, 'output 中文🌍');
            assert.equal(returned.extras.status, 'failed');
            assert.equal(returned.extras.loop_skipped, true);
            assert.deepEqual(returned.extras.error, { stage: 'invoke', message: 'distinct error' });
        }
    }
    assert.deepEqual(calls, originalCalls);
    assert.deepEqual({ results, messages }, originals);

    let sequence = 0;
    function item(event, data) {
        sequence++;
        return { kind: 'event', id: `event-${sequence}`, epoch: 'epoch', envelope: {
            ...workerEvent({ event, data, sequence }), hub_sequence: sequence,
        } };
    }
    const items = [item('run_started', {}), item('model_response', model), item('tool_calls', proposals)];
    const prompts = new Map([['approval', { confirmation_id: 'approval', settled_at: null, call: calls[0] }]]);
    const pending = buildRounds(items, prompts, new Map()).find(round => round.calls.length);
    assert.equal(pending.calls.length, 3); // No duplicate card for the model proposal and batch.
    assert.equal(pending.calls[0].status, 'pending');
    assert.equal(pending.calls[0].prompt.confirmation_id, 'approval');
    items.push(item('tool_results', normalizeDisplay('tool_results', messages)));
    const settled = buildRounds(items, new Map(), new Map()).find(round => round.calls.length);
    assert.deepEqual(settled.calls.map(call => call.id), calls.map(call => call.id));
    for (const call of settled.calls) {
        assert.equal(call.result.id, call.id);
        assert.equal(call.result.text, 'output 中文🌍');
        assert.equal(call.status, 'skipped');
        assert.equal(call.result.error.message, 'distinct error');
    }
});

it('bounds each tool body while retaining later batch identities and explicit omission counts', () => {
    const calls = Array.from({ length: 65 }, (_, index) => ({
        id: `call-${index}`, name: 'tool', type: 'serial_write', security: 'require_confirm',
        arguments: { raw: '\u0001'.repeat(65536) },
    }));
    const results = calls.map(query => ({ query, output: { type: 'text', modality: 'text', raw: '\u0001'.repeat(65536) },
        extras: { error: { stage: 'invoke', message: '\u0001'.repeat(65536) } } }));
    const proposed = normalizeDisplay('tool_calls', calls);
    const returned = normalizeDisplay('tool_results', results);
    assert.equal(proposed[63].id, 'call-63');
    assert.equal(returned[63].query.id, 'call-63');
    assert.equal(proposed.at(-1).omitted_items, 1);
    assert.equal(returned.at(-1).omitted_items, 1);
    assert.ok(Buffer.byteLength(JSON.stringify(proposed)) < 128 * 1024);
    assert.ok(Buffer.byteLength(JSON.stringify(returned)) < 512 * 1024);
    assert.equal(returned[0].output.truncated, true);
    assert.equal(returned[0].extras.error.display_truncated, true);
});

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

it('keeps whole-body omission sources live and replayable under a smaller transcript budget', async t => {
    const ctx = await setupPanelHub(t);
    ctx.config.limits.transcriptBytes = 64 * 1024;
    const session = ctx.hub.registry.create('omitted-answer');
    const worker = await ctx.connect(`/agent/${session.id}/events?token=${session.token}`);
    let sequence = 0;
    function send(event, data) {
        worker.send(workerEvent({ session: session.id, worker: 'w', sequence: ++sequence, event, data }));
    }
    send('status', { active: false, capabilities: ['answer-pages'] });
    await until(() => session.workerCapabilities?.names.includes('answer-pages'));
    const source = { worker_id: 'w', turn: 0, step: 0, commit_sequence: '1', fingerprint: 'a'.repeat(64) };
    const text = 'x'.repeat(70000);
    worker.ws.on('message', data => {
        const message = JSON.parse(data.toString());
        if (message.type !== 'payload' || message.data.operation !== 'answer') return;
        const { part, offset, request_id } = message.data;
        const raw = text.slice(offset, offset + 32768);
        const end = offset + raw.length;
        send('answer', { source, part, offset, request_id, raw, type: 'text', modality: 'text',
            next_offset: end, bytes: text.length, total_parts: 1, next_part: end === text.length ? 1 : 0,
            done: end === text.length });
    });
    const first = await ctx.panel();
    await ctx.subscribe(first.peer, session.id);
    send('model_response', { content: [{ type: 'text', modality: 'text', raw: text }], answer_source: source });
    const live = await first.peer.waitFor(message => message.type === 'event' && message.envelope.event === 'model_response');
    const replay = await ctx.panel();
    const subscribed = await ctx.subscribe(replay.peer, session.id);
    for (const envelope of [live.envelope, subscribed.transcript.find(event => event.event === 'model_response')]) {
        assert.equal(envelope.data.display_omitted, true);
        assert.equal(envelope.data.content, undefined);
        assert.deepEqual(envelope.data.answer_source, source);
        assert.ok(envelope.bytes < ctx.config.limits.transcriptBytes);
        let recovered = '';
        for (let offset = 0; offset < text.length;) {
            const query = { source: envelope.data.answer_source, part: 0, offset };
            const response = await fetch(`${ctx.base}/api/sessions/${session.id}/answer`, {
                method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(query) });
            assert.equal(response.status, 200);
            const page = await response.json();
            assert.ok(answerPage(page, query));
            recovered += page.raw;
            offset = page.next_offset;
        }
        assert.equal(recovered, text);
    }
});
