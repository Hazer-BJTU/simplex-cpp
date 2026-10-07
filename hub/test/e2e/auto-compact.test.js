/** Offline protocol acceptance through the real worker, Hub and panel store. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { rmSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';
import { sessionDir } from '../../src/launch/config-render.ts';
import { createPanelStore } from '../../web/src/state/store.ts';
import { buildRounds } from '../../web/src/app/rounds.ts';

it('keeps auto compact and private continuation inside one real worker run',
    { skip: e2eSkip, timeout: 60000 }, async () => {
        const ctx = await startE2eHub();
        const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
        const store = createPanelStore();
        let exchanges = 0;
        ctx.hub.mock.scenarioDelta = () => {
            exchanges += 1;
            if (exchanges === 1) return { toolCall: { index: 0, id: 'call-1', type: 'function',
                function: { name: 'unregistered_fixture', arguments: '{}' } } };
            if (exchanges === 2) return { text: 'Goal: finish the requested task. Verified: initial work. Next: answer.' };
            return { text: 'Final answer after automatic compaction.' };
        };
        const api = async (path, body) => {
            const res = await fetch(`${ctx.base}${path}`, { method: 'POST',
                headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
            assert.ok(res.ok);
            return res.json();
        };
        try {
            await api('/api/sessions', { session: 'auto-compact', spec: {
                provider: 'mock', model: 'mock-auto', autoCompactThreshold: 100000,
                maxExchanges: 1, maxAutoCompactions: 2,
            } });
            const started = await api('/api/sessions/auto-compact/start', {});
            assert.equal(started.ok, true, started.error);
            const session = ctx.hub.registry.get('auto-compact');
            await until(() => session.connected && session.workerCapabilities?.names.includes('auto-compact'),
                { timeout: 20000 });
            panel.send({ v: 1, type: 'subscribe', session: session.id });
            await panel.waitFor(message => message.type === 'subscribed');
            panel.send({ v: 1, type: 'input', session: session.id, request_id: 'task',
                content: [{ type: 'text', modality: 'text', raw: 'Task with historical details. '.repeat(2000) }] });
            const finished = await panel.waitFor(message => message.type === 'event'
                && message.envelope.event === 'run_finished' && message.envelope.request_id === 'task',
            { timeout: 30000 });
            assert.equal(finished.envelope.data.status, 'completed', JSON.stringify(finished));
            assert.equal(finished.envelope.data.exchanges, 3);
            assert.equal(finished.envelope.data.auto_compact.succeeded, 1);
            assert.equal(exchanges, 3);
            panel.send({ v: 1, type: 'history', session: session.id, request_id: 'inspect' });
            const history = await panel.waitFor(message => message.type === 'event'
                && message.envelope.event === 'history' && message.envelope.data.request_id === 'inspect');
            assert.deepEqual(history.envelope.data.turns[0].user, []);
            assert.equal(history.envelope.data.turns[0].internal_input, 'auto_compact_continue');
            assert.equal(history.envelope.data.turns[0].source.request_id, 'task');
            const snapshot = JSON.parse(readFileSync(join(sessionDir(ctx.config, session.id), 'state/state.json'), 'utf8'));
            assert.match(snapshot.turns[0].user_input.content[0].raw, /Continue the user's unfinished task/);
            for (const message of panel.messages) {
                if (message.type === 'welcome') store.getState().applyWelcome(message);
                if (message.type === 'subscribed') store.getState().applySubscribed(message);
                if (message.type === 'event') store.getState().applyEvent(message);
            }
            const view = store.getState().views.get(session.id);
            const rounds = buildRounds(view.items, view.confirmations, view.requests).filter(round => round.kind === 'run');
            assert.equal(rounds.length, 1);
            assert.equal(rounds[0].compacting, false);
            assert.equal(rounds[0].calls.find(call => call.name === 'auto_compact').result !== null, true);
            const events = panel.messages.filter(message => message.type === 'event').map(message => message.envelope);
            assert.equal(events.filter(event => event.event === 'run_started').length, 1);
            assert.equal(events.filter(event => event.event === 'run_finished').length, 1);
            assert.equal(events.filter(event => event.event === 'input_committed').length, 1);
            assert.equal(JSON.stringify(events).includes("Continue the user's unfinished task"), false);
            // A new explicit request resets counters; old memory/status do not
            // schedule another compaction before its ordinary model response.
            panel.send({ v: 1, type: 'input', session: session.id, request_id: 'next-task',
                content: [{ type: 'text', modality: 'text', raw: 'A new task.' }] });
            const next = await panel.waitFor(message => message.type === 'event'
                && message.envelope.event === 'run_finished' && message.envelope.request_id === 'next-task',
            { timeout: 30000 });
            assert.equal(next.envelope.data.status, 'completed');
            assert.equal(next.envelope.data.exchanges, 1);
            assert.equal(next.envelope.data.auto_compact.attempts, 0);
            assert.equal(next.envelope.data.auto_compact.succeeded, 0);

        } finally {
            await panel.close();
            await ctx.hub.stop();
            rmSync(ctx.config.dataDir, { recursive: true, force: true });
        }
    });
