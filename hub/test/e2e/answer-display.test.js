/** Complete SSE -> state -> snapshot -> replay, with bounded display delivery. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { once } from 'node:events';
import { readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';
import { sessionDir } from '../../src/launch/config-render.ts';
import { answerPage } from '../../shared/answers.ts';
import { buildRounds } from '../../web/src/app/rounds.ts';

it('preserves large accepted SSE content and reconstructs the same answer after reconnect/history',
    { skip: e2eSkip, timeout: 60000 }, async () => {
        const ctx = await startE2eHub();
        const answer = 'final 中文🌍\n'.repeat(40000);
        const reasoning = 'reasoning 中文🌍\n'.repeat(120000);
        const requests = [];
        ctx.hub.mock.handle = async (req, res) => {
            const chunks = [];
            for await (const data of req) chunks.push(data);
            requests.push(JSON.parse(Buffer.concat(chunks).toString('utf8')));
            const delta = requests.length === 1
                ? { role: 'assistant', content: answer, reasoning_content: reasoning }
                : { role: 'assistant', content: 'Replay verified.' };
            const frames = [{ id: 'large-completed', choices: [{ index: 0, delta, finish_reason: null }] },
                { id: 'large-completed', choices: [{ index: 0, delta: {}, finish_reason: 'stop' }],
                    usage: { prompt_tokens: 100, completion_tokens: 65536, total_tokens: 65636 } }];
            const bytes = Buffer.from(frames.map(frame => `data: ${JSON.stringify(frame)}\n\n`).join('') + 'data: [DONE]\n\n');
            res.writeHead(200, { 'Content-Type': 'text/event-stream' });
            for (let offset = 0; offset < bytes.length; offset += 8191) {
                if (!res.write(bytes.subarray(offset, offset + 8191))) await once(res, 'drain');
            }
            res.end();
        };
        const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
        const api = async (path, body) => {
            const response = await fetch(`${ctx.base}${path}`, { method: 'POST',
                headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
            const value = await response.json();
            assert.ok(response.ok, JSON.stringify(value));
            return value;
        };
        try {
            await api('/api/sessions', { session: 'large-output', spec: { provider: 'mock', model: 'mock-text' } });
            const start = await api('/api/sessions/large-output/start', {});
            assert.equal(start.ok, true, start.error);
            const session = ctx.hub.registry.get('large-output');
            await until(() => session.workerCapabilities?.names.includes('answer-pages'), { timeout: 20000 });
            panel.send({ v: 1, type: 'subscribe', session: session.id });
            await panel.waitFor(message => message.type === 'subscribed');
            panel.send({ v: 1, type: 'input', session: session.id, request_id: 'large-task',
                content: [{ type: 'text', modality: 'text', raw: 'Complete the synthetic task.' }] });
            const live = await panel.waitFor(message => message.type === 'event' && message.envelope.event === 'model_response', { timeout: 20000 });
            await panel.waitFor(message => message.type === 'event' && message.envelope.event === 'run_finished', { timeout: 20000 });
            assert.ok(Buffer.byteLength(JSON.stringify(live)) < 1024 * 1024);
            assert.equal(live.envelope.data.reasoning.truncated, true);
            const source = live.envelope.data.answer_source;
            let query = { source, part: 0, offset: 0 };
            let complete = '';
            for (let count = 0;; ++count) {
                assert.ok(count < 100);
                const page = await api(`/api/sessions/${session.id}/answer`, query);
                assert.ok(answerPage(page, query));
                complete += page.raw;
                if (page.done) break;
                query = { source, part: page.next_part, offset: page.next_part === query.part ? page.next_offset : 0 };
            }
            assert.equal(complete, answer);
            const snapshotPath = join(sessionDir(ctx.config, session.id), 'state/state.json');
            const snapshot = JSON.parse(readFileSync(snapshotPath, 'utf8'));
            assert.equal(snapshot.turns[0].agent_loop_step[0].model_response.content[0].raw, answer);
            assert.equal(snapshot.turns[0].agent_loop_step[0].model_response.reasoning.raw, reasoning);
            panel.ws.terminate();
            const replay = await connectWorker(`${ctx.wsBase}/panel/ws`);
            try {
                replay.send({ v: 1, type: 'subscribe', session: session.id });
                const subscribed = await replay.waitFor(message => message.type === 'subscribed');
                assert.deepEqual(subscribed.transcript.find(event => event.event === 'model_response').data.answer_source, source);
                replay.send({ v: 1, type: 'history', session: session.id, request_id: 'after-reconnect' });
                const history = await replay.waitFor(message => message.type === 'event' && message.envelope.event === 'history');
                assert.deepEqual(history.envelope.data.turns[0].steps[0].answer_source, source);
                replay.send({ v: 1, type: 'input', session: session.id, request_id: 'verify-replay',
                    content: [{ type: 'text', modality: 'text', raw: 'Continue from the full answer.' }] });
                await replay.waitFor(message => message.type === 'event' && message.envelope.event === 'run_finished'
                    && message.envelope.request_id === 'verify-replay', { timeout: 20000 });
                const previous = requests[1].messages.find(message => message.role === 'assistant');
                assert.equal(previous.content, answer);
                if (previous.reasoning_content !== undefined) assert.equal(previous.reasoning_content, reasoning);
            } finally { replay.ws.terminate(); }
        } finally {
            panel.ws.terminate();
            await ctx.hub.stop();
            rmSync(ctx.config.dataDir, { recursive: true, force: true });
        }
});

it('projects 100 real worker calls through Hub normalization into 64 panel cards and exact notices',
    { skip: e2eSkip, timeout: 60000 }, async () => {
        const ctx = await startE2eHub();
        let requests = 0;
        ctx.hub.mock.handle = async (req, res) => {
            for await (const chunk of req) { /* Drain the provider request. */ }
            const first = requests++ === 0;
            const delta = first ? { role: 'assistant', tool_calls: Array.from({ length: 100 }, (_, index) => ({
                index, id: `batch-call-${index}`, type: 'function',
                // Unknown tools fail before invocation: this regression has no tool side effects.
                function: { name: `missing_tool_${index}`, arguments: '{}' },
            })) } : { role: 'assistant', content: 'The batch has settled.' };
            res.writeHead(200, { 'Content-Type': 'text/event-stream' });
            res.end(`data: ${JSON.stringify({ id: 'batch-100', choices: [{ index: 0, delta,
                finish_reason: first ? 'tool_calls' : 'stop' }],
                usage: { prompt_tokens: 100, completion_tokens: 100, total_tokens: 200 } })}\n\ndata: [DONE]\n\n`);
        };
        const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
        const api = async (path, body) => {
            const response = await fetch(`${ctx.base}${path}`, { method: 'POST',
                headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
            const value = await response.json();
            assert.ok(response.ok, JSON.stringify(value));
            return value;
        };
        try {
            await api('/api/sessions', { session: 'batch-output', spec: { provider: 'mock', model: 'mock-text' } });
            const started = await api('/api/sessions/batch-output/start', {});
            assert.equal(started.ok, true, started.error);
            const session = ctx.hub.registry.get('batch-output');
            await until(() => session.connected, { timeout: 20000 });
            panel.send({ v: 1, type: 'subscribe', session: session.id });
            await panel.waitFor(message => message.type === 'subscribed');
            panel.send({ v: 1, type: 'input', session: session.id, request_id: 'batch-task',
                content: [{ type: 'text', modality: 'text', raw: 'Exercise the synthetic tool batch.' }] });
            await panel.waitFor(message => message.type === 'event' && message.envelope.event === 'run_finished'
                && message.envelope.request_id === 'batch-task', { timeout: 20000 });
            const events = panel.messages.filter(message => message.type === 'event'
                && message.envelope.request_id === 'batch-task').map(message => message.envelope);
            for (const data of [events.find(event => event.event === 'model_response').data.invokes,
                events.find(event => event.event === 'tool_calls').data,
                events.find(event => event.event === 'tool_results').data]) {
                assert.equal(data.length, 65);
                assert.deepEqual(data.at(-1), { display_omitted: true, omitted_items: 36 });
            }
            const snapshot = JSON.parse(readFileSync(join(sessionDir(ctx.config, session.id), 'state/state.json'), 'utf8'));
            assert.equal(snapshot.turns[0].agent_loop_step[0].model_response.invokes.length, 100);
            assert.equal(snapshot.turns[0].agent_loop_step[0].invoke_returns.length, 100);
            const items = events.map((envelope, index) => ({ kind: 'event', id: `batch-${index}`, epoch: 'test', envelope }));
            const round = buildRounds(items, new Map(), new Map()).find(value => value.calls.length);
            assert.equal(round.calls.length, 64);
            assert.ok(round.calls.every(call => call.id.startsWith('batch-call-')
                && call.name.startsWith('missing_tool_') && call.result.id === call.id && call.status === 'failed'));
            assert.deepEqual(round.timeline.filter(entry => entry.kind === 'tool_omission')
                .map(({ category, count }) => ({ category, count })), [
                { category: 'calls', count: 36 }, { category: 'results', count: 36 },
            ]);
        } finally {
            panel.ws.terminate();
            await ctx.hub.stop();
            rmSync(ctx.config.dataDir, { recursive: true, force: true });
        }
    });
