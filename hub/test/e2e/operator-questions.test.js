import assert from 'node:assert/strict';
import { rmSync } from 'node:fs';
import { it } from 'node:test';
import { e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';

it('settles real operator waits by answer, targeted cancel and worker shutdown',
    { skip: e2eSkip, timeout: 60000 }, async t => {
        const ctx = await startE2eHub();
        const panel = await connectWorker(ctx.wsBase + '/panel/ws');
        t.after(async () => {
            await panel.close();
            await ctx.hub.stop();
            rmSync(ctx.config.dataDir, { recursive: true, force: true });
        });
        const session = ctx.hub.registry.create('questions', { provider: 'mock', model: 'mock-auto' });
        ctx.hub.mock.scenarioDelta = (_scenario, body) => {
            const last = body.messages.at(-1);
            if (last.role === 'tool') return { text: last.content };
            return { toolCall: { index: 0, id: 'ask-' + ctx.hub.mock.requests.length,
                type: 'function', function: { name: 'ask_user',
                    arguments: JSON.stringify({ question: 'Which path?' }) } } };
        };
        const started = await ctx.hub.supervisor.start(session);
        assert.equal(started.ok, true, started.error);
        await until(() => session.connected && session.workerCapabilities, { timeout: 20000 })
            .catch(error => { throw new Error(ctx.hub.supervisor.logs(session).join('\n'), { cause: error }); });
        panel.send({ type: 'subscribe', session: session.id });
        await panel.waitFor(m => m.type === 'subscribed');
        for (const action of ['answer', 'cancel', 'shutdown']) {
            const from = panel.messages.length;
            panel.send({ type: 'input', session: session.id, request_id: action,
                content: [{ type: 'text', modality: 'text', raw: 'Ask me a question' }] });
            const pending = () => panel.messages.slice(from).find(m => m.type === 'questions' && m.questions.length);
            await until(pending, { timeout: 5000, label: 'pending operator question' });
            if (action === 'answer') {
                panel.send({ type: 'question_answer', session: session.id,
                    question_id: pending().questions[0].question_id, answer: { text: 'Take path B' } });
            } else if (action === 'cancel') {
                panel.send({ type: 'signal', session: session.id, operation: 'cancel', run_id: session.activeRunId });
            } else {
                // Bypass Hub question cancellation to exercise the worker-local stop token.
                session.connection.sendSignal({ type: 'signal', data: { operation: 'shutdown' } });
                await until(() => !session.connected, { timeout: 5000, label: 'worker shutdown during ask' });
                break;
            }
            const terminal = await panel.waitFor(m => m.envelope?.event === 'run_finished'
                && m.envelope.request_id === action, { timeout: 5000 });
            assert.equal(terminal.envelope.data.status, action === 'answer' ? 'completed' : 'cancelled');
            if (action === 'answer') assert.match(JSON.stringify(ctx.hub.mock.requests.at(-1)), /Take path B/);
        }
    });
