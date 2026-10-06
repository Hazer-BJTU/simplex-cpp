/** Drive the actual intrinsic tools through deterministic Chat Completions calls. */
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { it } from 'node:test';
import { startE2eHub, e2eSkip } from '../helpers/e2e.js';
import { until } from '../helpers/worker.js';
import { sessionDir } from '../../src/launch/config-render.ts';

it('the real worker forks, sends, receives, continues, compacts and stops through its intrinsic tools',
    { skip: e2eSkip, timeout: 60000 }, async t => {
        const ctx = await startE2eHub();
        t.after(() => ctx.hub.stop());
        const parent = ctx.hub.registry.create('intrinsic-subagent-parent',
            { provider: 'mock', model: 'mock-text' });
        const marker = 'delegate-via-real-intrinsic-tools';
        let phase = 'fork';
        let childId;
        let childRequest;
        let sequence = 0;
        const calls = [];
        const responses = [];
        const invoke = (name, args) => {
            calls.push({ name, args });
            return { toolCall: { index: 0, id: `subagent-tool-${++sequence}`, type: 'function',
                function: { name, arguments: JSON.stringify(args) } } };
        };
        // A small fixture model delay allows independent worker/event channels to
        // progress. The tool itself remains a one-shot snapshot, never a long poll.
        const handle = ctx.hub.mock.handle.bind(ctx.hub.mock);
        ctx.hub.mock.handle = async (req, res) => {
            await new Promise(resolve => setTimeout(resolve, 75));
            await handle(req, res);
        };
        ctx.hub.mock.scenarioDelta = (_scenario, body) => {
            const lastUser = [...body.messages].reverse().find(message => message.role === 'user');
            if (!JSON.stringify(lastUser?.content).includes(marker)) {
                return { text: 'offline child answer 🌻' };
            }
            const result = [...body.messages].reverse().find(message => message.role === 'tool');
            const text = typeof result?.content === 'string' ? result.content : '';
            if (result) {
                responses.push(text);
                assert.doesNotMatch(text, /invalid subagent result|subagent exchange failed|hub rejected subagent request/);
            }
            assert.ok(sequence < 100, 'delegation fixture must make bounded progress');
            switch (phase) {
                case 'fork':
                    phase = 'ready';
                    return invoke('subagent_fork', {});
                case 'ready': {
                    childId ??= text.match(/\[\[subagent_id\]\]: (subagent-[\w-]+)/)?.[1];
                    assert.ok(childId, `fork must return a child ID: ${text}`);
                    if (!text.includes('[[lifecycle]]: ready') || !text.includes('[[connected]]: true')) {
                        return invoke('subagent_receive', { subagent_id: childId });
                    }
                    const child = ctx.hub.registry.require(childId);
                    assert.notEqual(child.identity.workerId, parent.identity.workerId);
                    assert.equal(child.subagent.policy, 'ask');
                    assert.equal(ctx.hub.subagents.children.get(childId).conversation.value.turns.length, 0);
                    phase = 'message-sent';
                    return invoke('subagent_send', { subagent_id: childId, operation: 'message',
                        content: [{ type: 'text', modality: 'text',
                            // Compaction requires a measurable context reduction,
                            // so give the child enough disposable history.
                            raw: 'Return the offline child answer. ' + 'Supporting context. '.repeat(2000) }] });
                }
                case 'message-sent':
                case 'continue-sent':
                case 'compact-sent':
                    childRequest = text.match(/\[\[request_id\]\]: ([\w-]+)/)?.[1];
                    assert.ok(childRequest, text);
                    assert.match(text, /\[\[state\]\]: sent/);
                    phase = phase.replace('-sent', '-received');
                    return invoke('subagent_receive', { subagent_id: childId });
                case 'message-received':
                case 'continue-received':
                case 'compact-received': {
                    const outcome = text.split('\n---\n\n').find(record =>
                        record.includes(`[[request_id]]: ${childRequest}\n`)
                        && record.includes('[[state]]:')) ?? '';
                    if (outcome.includes('[[state]]: finished')) {
                        assert.match(outcome, /\[\[run_status\]\]: completed/, 'child operation failed');
                    }
                    if (!outcome.includes('[[state]]: finished')
                        || !outcome.includes('[[run_status]]: completed')
                        || !text.includes('offline child answer')
                        || (phase === 'compact-received' && !outcome.includes('compact summary'))) {
                        return invoke('subagent_receive', { subagent_id: childId });
                    }
                    const record = ctx.hub.subagents.children.get(childId);
                    if (phase === 'message-received') {
                        assert.equal(record.conversation.value.turns.length, 1);
                        phase = 'continue-sent';
                        return invoke('subagent_send', { subagent_id: childId, operation: 'continue' });
                    }
                    if (phase === 'continue-received') {
                        assert.equal(record.conversation.value.turns.length, 1,
                            'continue must resume without an empty user turn');
                        phase = 'compact-sent';
                        return invoke('subagent_send', { subagent_id: childId, operation: 'compact' });
                    }
                    assert.match(text, /compact summary/);
                    phase = 'stopping';
                    return invoke('subagent_send', { subagent_id: childId, operation: 'stop' });
                }
                case 'stopping':
                    assert.match(text, /\[\[operation_id\]\]:/);
                    assert.match(text, /\[\[state\]\]: stopping/);
                    phase = 'complete';
                    return { text: 'delegation complete' };
                default:
                    return { text: 'delegation complete' };
            }
        };
        const started = await ctx.hub.supervisor.start(parent);
        assert.equal(started.ok, true, started.error);
        await until(() => parent.workerCapabilities?.names.includes('session-history'), { timeout: 10000 });
        parent.connection.sendPayload({ type: 'payload', data: {
            operation: 'message', request_id: 'delegate-request',
            content: [{ type: 'text', modality: 'text', raw: marker }],
        } });
        await until(() => phase === 'complete' && !parent.activeRunId, { timeout: 40000 });
        assert.equal(ctx.hub.mock.failures.length, 0);
        assert.equal(calls.filter(call => call.name === 'subagent_fork').length, 1);
        assert.deepEqual(calls.filter(call => call.name === 'subagent_send')
            .map(call => call.args.operation), ['message', 'continue', 'compact', 'stop']);
        assert.ok(responses.some(text => text.includes('[[requests_truncated]]: false')));
        assert.ok(responses.some(text => text.includes('offline child answer 🌻')));
        const parentState = readFileSync(`${sessionDir(ctx.config, parent.id)}/state/state.json`, 'utf8');
        assert.match(parentState, /delegation complete/);
        const state = JSON.parse(parentState);
        for (const name of ['subagent_fork', 'subagent_send', 'subagent_receive']) {
            assert.ok(state.tools.some(tool => tool.name === name));
        }
        await until(() => !existsSync(sessionDir(ctx.config, childId)), { timeout: 10000 });
        assert.equal(ctx.hub.transcripts.transcripts.has(childId), false);
        assert.equal((await ctx.hub.supervisor.stop(parent)).ok, true);
    });
