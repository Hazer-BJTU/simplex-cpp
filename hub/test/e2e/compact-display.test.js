/** Complete compact text through the real C++ worker, Hub, replay and panel store. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';
import { compactSummary } from '../helpers/compact.js';
import { sessionDir } from '../../src/launch/config-render.ts';
import { createPanelStore } from '../../web/src/state/store.ts';
import { buildRounds } from '../../web/src/app/rounds.ts';

for (const bytes of [8192, 32768]) {
    it(`preserves a ${bytes}-byte committed compact summary through live and replay delivery`,
        { skip: e2eSkip, timeout: 60000 }, async t => {
            const ctx = await startE2eHub();
            // Leave room for a near-limit summary alongside the installed tool
            // declarations. Compact's separate context-budget policy is unchanged.
            ctx.config.worker.systemPromptFile = 'prompts/general_agent.yaml';
            const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
            t.after(async () => {
                await panel.close();
                await ctx.hub.stop();
                rmSync(ctx.config.dataDir, { recursive: true, force: true });
            });
            const summary = compactSummary(bytes);
            let exchanges = 0;
            ctx.hub.mock.scenarioDelta = () => ({ text: ++exchanges === 1 ? 'Original answer.' : summary });
            const session = ctx.hub.registry.create('compact-summary', { provider: 'mock', model: 'mock-text' });
            const started = await ctx.hub.supervisor.start(session);
            assert.equal(started.ok, true, started.error);
            await until(() => session.workerCapabilities?.names.includes('context-compact'), { timeout: 20000 });
            panel.send({ v: 1, type: 'subscribe', session: session.id });
            await panel.waitFor(message => message.type === 'subscribed');
            panel.send({ v: 1, type: 'input', session: session.id, request_id: 'task',
                content: [{ type: 'text', modality: 'text', raw: 'Historical context. '.repeat(5000) }] });
            const ordinary = await panel.waitFor(message => message.type === 'event'
                && message.envelope.event === 'run_finished' && message.envelope.request_id === 'task', { timeout: 20000 });
            assert.equal(ordinary.envelope.data.status, 'completed');
            panel.send({ v: 1, type: 'input', session: session.id, request_id: 'compact', operation: 'compact' });
            const finished = await panel.waitFor(message => message.type === 'event'
                && message.envelope.event === 'run_finished' && message.envelope.request_id === 'compact', { timeout: 20000 });
            assert.equal(finished.envelope.data.status, 'completed', JSON.stringify(finished));
            const live = panel.messages.find(message => message.type === 'event'
                && message.envelope.event === 'compact_finished');
            assert.equal(live.envelope.data.summary, summary);
            assert.equal(live.envelope.data.display_truncated, undefined);
            const saved = JSON.parse(readFileSync(join(sessionDir(ctx.config, session.id), 'state/state.json'), 'utf8'));
            assert.deepEqual(saved.turns, []);
            assert.ok(JSON.stringify(saved).includes(JSON.stringify(summary).slice(1, -1)));
            // A new panel gets the same normalized summary from bounded replay.
            const replay = await connectWorker(`${ctx.wsBase}/panel/ws`);
            t.after(() => replay.close());
            replay.send({ v: 1, type: 'subscribe', session: session.id });
            const subscribed = await replay.waitFor(message => message.type === 'subscribed');
            assert.equal(subscribed.transcript.find(event => event.event === 'compact_finished').data.summary, summary);
            const store = createPanelStore();
            store.getState().applySubscribed(subscribed);
            const view = store.getState().views.get(session.id);
            const round = buildRounds(view.items, view.confirmations, view.requests).find(round => round.compactResult);
            assert.equal(round.compactResult.summary, summary);
        });
}
