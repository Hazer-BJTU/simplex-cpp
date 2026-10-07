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
import { reconcileInternalHistory } from '../../web/src/app/history-rounds.ts';

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

for (const restart of [false, true]) {
    it(`restores per-response execution after failed automatic continuation, restart=${restart}`,
        { skip: e2eSkip, timeout: 60000 }, async () => {
            const ctx = await startE2eHub();
            const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
            let exchanges = 0;
            let resume = false;
            ctx.hub.mock.scenarioDelta = () => {
                if (resume) return { text: 'Answer from explicit Continue.' };
                exchanges += 1;
                if (exchanges === 3) return { text: 'Goal: finish the task. State: initial work done. Next: continue.' };
                if (exchanges >= 5) throw new Error('injected continuation failure');
                return { toolCall: { index: 0, id: `call-${exchanges}`, type: 'function',
                    function: { name: 'unregistered_fixture', arguments: '{}' } } };
            };
            const api = async (path, body) => {
                const res = await fetch(`${ctx.base}${path}`, { method: 'POST',
                    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
                assert.ok(res.ok);
                return res.json();
            };
            try {
                await api('/api/sessions', { session: 'auto-resume', spec: {
                    provider: 'mock', model: 'mock-auto', autoCompactThreshold: 100000,
                    maxExchanges: 2, maxAutoCompactions: 2,
                } });
                assert.equal((await api('/api/sessions/auto-resume/start', {})).ok, true);
                const session = ctx.hub.registry.get('auto-resume');
                await until(() => session.connected, { timeout: 20000 });
                panel.send({ v: 1, type: 'subscribe', session: session.id });
                await panel.waitFor(message => message.type === 'subscribed');
                panel.send({ v: 1, type: 'input', session: session.id, request_id: 'A',
                    content: [{ type: 'text', modality: 'text', raw: 'Detailed task. '.repeat(3000) }] });
                const failed = await panel.waitFor(message => message.type === 'event'
                    && message.envelope.event === 'run_finished' && message.envelope.request_id === 'A',
                { timeout: 30000 });
                assert.equal(failed.envelope.data.status, 'failed');
                assert.equal(failed.envelope.data.auto_compact.succeeded, 1);
                if (restart) {
                    assert.equal((await api(`/api/sessions/${session.id}/stop`, {})).ok, true);
                    await until(() => !session.connected, { timeout: 20000 });
                    assert.equal((await api(`/api/sessions/${session.id}/start`, {})).ok, true);
                    await until(() => session.connected && session.identity.workerId !== failed.envelope.worker_id,
                        { timeout: 20000 });
                }
                resume = true;
                panel.send({ v: 1, type: 'input', session: session.id, request_id: 'B', operation: 'continue' });
                const finished = await panel.waitFor(message => message.type === 'event'
                    && message.envelope.event === 'run_finished' && message.envelope.request_id === 'B',
                { timeout: 30000 });
                assert.equal(finished.envelope.data.status, 'completed');
                panel.send({ v: 1, type: 'history', session: session.id, request_id: 'inspect-resume' });
                const history = await panel.waitFor(message => message.type === 'event'
                    && message.envelope.event === 'history'
                    && message.envelope.data.request_id === 'inspect-resume');
                const turn = history.envelope.data.turns[0];
                assert.equal(turn.source.request_id, 'A');
                assert.deepEqual(turn.user, []);
                assert.equal(turn.steps.length, 2);
                for (const [index, event] of [failed, finished].entries()) {
                    assert.deepEqual(turn.steps[index].execution, { worker_id: event.envelope.worker_id,
                        request_id: event.envelope.request_id, run_id: event.envelope.run_id });
                }
                assert.equal(turn.steps[0].execution.worker_id === turn.steps[1].execution.worker_id, !restart);
                const snapshot = JSON.parse(readFileSync(join(sessionDir(ctx.config, session.id), 'state/state.json'), 'utf8'));
                assert.deepEqual(snapshot.turns[0].agent_loop_step.map(step => step.extras['simplex.execution']),
                    turn.steps.map(step => step.execution));
                const events = panel.messages.filter(message => message.type === 'event').map(message => message.envelope);
                for (const missing of [null, turn.steps[0].commit_sequence, turn.steps[1].commit_sequence]) {
                    const replay = events.filter(event => event.event !== 'history'
                        && !(event.event === 'model_response' && event.data.commit_sequence === missing));
                    const store = createPanelStore();
                    store.getState().applySubscribed({ type: 'subscribed', session: session.describe(),
                        transcript: replay, logs: [], latest: replay.at(-1).hub_sequence });
                    const view = store.getState().views.get(session.id);
                    const runs = buildRounds(view.items, view.confirmations, view.requests)
                        .filter(round => round.kind === 'run');
                    const restored = reconcileInternalHistory([turn], runs,
                        history.envelope.sequence, history.envelope.worker_id);
                    assert.equal(restored.history.length, 0);
                    assert.deepEqual(restored.rounds.map(round => round.assistant.length), [3, 1]);
                    assert.equal(restored.rounds[1].assistant[0].text, 'Answer from explicit Continue.');
                    assert.equal(restored.rounds[0].calls.length, 4); // three model calls plus automatic compact
                    assert.equal(restored.rounds[1].input, null);
                    assert.equal(JSON.stringify(events).includes("Continue the user's unfinished task"), false);
                }
            } finally {
                await panel.close();
                await ctx.hub.stop();
                rmSync(ctx.config.dataDir, { recursive: true, force: true });
            }
        });
}
