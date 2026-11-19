/** Inspect the actual Chat Completions payloads, stored state and Hub events. */
import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { it } from 'node:test';
import { renderWorkerConfig, sessionDir, workerConfigPath } from '../../src/launch/config-render.ts';
import { normalizeSpec } from '../../src/launch/spec.ts';
import { e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';

const marker = '[Runtime budget]';
for (const trigger of ['steps', 'tokens', 'large token limit']) {
    it('shows the ' + trigger + ' reminder only after a fresh tool result', { skip: e2eSkip }, async t => {
        const ctx = await startE2eHub();
        const panel = await connectWorker(ctx.wsBase + '/panel/ws');
        t.after(async () => {
            await panel.close();
            await ctx.hub.stop();
            rmSync(ctx.config.dataDir, { recursive: true, force: true });
        });
        const seen = [];
        const large = trigger === 'large token limit';
        const limit = large ? 1_000_000_000_000_000_000n : 9n;
        // Reach ceil(limit * percent / 100) exactly on the second exchange.
        const percent = large ? 81n : 40n;
        const atThreshold = (limit * percent + 99n) / 100n;
        ctx.hub.mock.handle = async (req, res) => {
            seen.push(await ctx.hub.mock.readBody(req));
            const n = seen.length;
            // Exchange 2 reaches the threshold exactly with a tool batch.
            // Exchange 3 only reasons, so its successor must omit the reminder.
            const call = i => ({ index: i, id: 'call-' + n + '-' + i, type: 'function',
                function: { name: 'plan', arguments: '{"operation":"read"}' } });
            const tools = [1, 2, 4].includes(n);
            const delta = tools ? { tool_calls: [call(0), call(1)] }
                : n === 3 ? { reasoning_content: 'considering the next action' } : { content: 'finished' };
            const frame = { choices: [{ index: 0, delta, finish_reason: tools ? 'tool_calls' : 'stop' }],
                usage: { prompt_tokens: 10, completion_tokens: 2 } };
            const wire = large && n === 2
                ? JSON.stringify(frame).replace('"completion_tokens":2', '"completion_tokens":' + (atThreshold - 2n))
                : JSON.stringify(frame);
            res.writeHead(200, { 'content-type': 'text/event-stream' })
                .end('data: ' + wire + '\n\ndata: [DONE]\n\n');
        };
        const spec = { provider: 'mock', model: 'mock-text' };
        const session = ctx.hub.registry.create('budget-' + trigger.replaceAll(' ', '-'), spec);
        const config = renderWorkerConfig({ config: ctx.config, sessionId: session.id,
            spec: normalizeSpec(ctx.config, spec), endpoints: ctx.hub.supervisor.endpointsFor(session.id, session.token),
            mock: { baseUrl: ctx.hub.mock.baseUrl } });
        config.experimental = {
            cumulative_step_control: trigger === 'steps' ? 4 : 0,
            cumulative_token_control: { type: 'generated', threshold: Number(limit) },
            model_visible_budget: trigger === 'steps' ? { after_exchanges: 2 } : { token_percent: Number(percent) },
        };
        const path = workerConfigPath(ctx.config, session.id);
        mkdirSync(dirname(path), { recursive: true });
        writeFileSync(path, JSON.stringify(config));
        const started = await ctx.hub.supervisor.start(session);
        assert.equal(started.ok, true, started.error);
        await until(() => session.connected && session.workerCapabilities, { timeout: 15000 });
        panel.send({ type: 'subscribe', session: session.id });
        await panel.waitFor(m => m.type === 'subscribed');
        async function run(id) {
            panel.send({ type: 'input', session: session.id, request_id: id,
                content: [{ type: 'text', raw: 'Complete the fixture task', modality: 'text' }] });
            const { envelope } = await panel.waitFor(m => m.envelope?.event === 'run_finished'
                && m.envelope.request_id === id, { timeout: 20000 });
            assert.equal(envelope.data.status, 'completed', JSON.stringify(envelope.data));
            return envelope.data.experimental;
        }
        assert.equal((await run('first')).steps, 5);
        assert.equal((await run('second')).steps, 1);
        assert.equal(seen.length, 6);
        for (const [index, request] of seen.entries()) {
            assert.deepEqual(request.messages[0], seen[0].messages[0]);
            const notices = request.messages.filter(m => JSON.stringify(m).includes(marker));
            if (index !== 2 && index !== 4) {
                assert.deepEqual(notices, []);
                continue;
            }
            const last = request.messages.at(-1);
            assert.deepEqual(notices, [last]);
            assert.equal(last.role, 'tool');
            assert.equal(last.tool_call_id, 'call-' + index + '-1');
            if (trigger === 'steps') {
                assert.match(last.content, index === 2 ? /within 2 more steps/ : /final step/);
            } else {
                assert.doesNotMatch(last.content, /more steps|final step/);
            }
            assert.ok(last.content.includes((limit - atThreshold - BigInt(index - 2) * 2n) + ' generated tokens'));
            assert.match(last.content, /settling that reply's tool calls\.$/);
        }
        assert.ok(!JSON.stringify(panel.messages).includes(marker));
        const saved = readFileSync(join(sessionDir(ctx.config, session.id), 'state/state.json'), 'utf8');
        assert.ok(!saved.includes(marker) && !saved.includes('simplex.tool_result_notice'));
    });
}
