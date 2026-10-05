/** The Hub-only API works with the unchanged real C++ worker and offline provider. */
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { it } from 'node:test';
import { startE2eHub, e2eSkip } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';
import { sessionDir } from '../../src/launch/config-render.ts';

it('clean-forks a real worker, delegates one message and observes its primary history',
    { skip: e2eSkip, timeout: 30000 }, async t => {
        const ctx = await startE2eHub({ mock: { enabled: true, slowMs: 2500 } });
        t.after(() => ctx.hub.stop());
        const parent = ctx.hub.registry.create('real-subagent-parent', { provider: 'mock', model: 'mock-slow' });
        const started = await ctx.hub.supervisor.start(parent);
        assert.equal(started.ok, true, started.error);
        await until(() => parent.workerCapabilities?.names.includes('session-history'), { timeout: 5000 });
        parent.connection.sendPayload({ type: 'payload', data: { operation: 'message', request_id: 'parent-task',
            content: [{ type: 'text', modality: 'text', raw: 'Keep this request active for the delegation fixture.' }] } });
        await until(() => !!parent.activeRunId, { timeout: 5000 });
        async function call(route, arguments_) {
            const endpoint = new URL(ctx.hub.supervisor.endpointsFor(parent.id, parent.token).tools);
            endpoint.pathname += `/${route}`;
            const peer = await connectWorker(endpoint.href);
            peer.send({ type: 'tool_request', data: { worker_id: parent.identity.workerId,
                session_id: parent.id, run_id: parent.activeRunId, request_id: `request-${route}`,
                arguments: arguments_ } });
            const result = (await peer.waitFor(message => message.type === 'tool_response')).data;
            await peer.waitForClose();
            assert.equal(result.status, 'succeeded', JSON.stringify(result));
            return result.result;
        }
        const created = await call('subagent/clean-fork', {});
        const child = ctx.hub.registry.require(created.subagent_id);
        await until(() => child.subagent.lifecycle === 'ready', { timeout: 5000 });
        assert.equal(child.identity.workerId !== parent.identity.workerId, true);
        const sent = await call('subagent/send', { subagent_id: child.id, operation: 'message',
            content: [{ type: 'text', modality: 'text', raw: 'Give one short answer.' }] });
        assert.equal(sent.state, 'sent');
        const record = ctx.hub.subagents.children.get(child.id);
        await until(() => record.conversation.value.turns[0]?.steps.some(step => step.content.length)
            && !record.conversation.value.stale && !child.activeRunId, { timeout: 10000 });
        assert.equal(record.conversation.value.turns[0].user[0].raw, 'Give one short answer.');
        assert.equal(ctx.hub.transcripts.transcripts.has(child.id), false);
        assert.equal(existsSync(`${sessionDir(ctx.config, child.id)}/events.jsonl`), false);
        assert.equal((await ctx.hub.supervisor.stop(parent)).ok, true);
        assert.equal(existsSync(sessionDir(ctx.config, child.id)), false);
    });
