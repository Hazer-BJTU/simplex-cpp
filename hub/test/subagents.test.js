/** Real process/socket coverage of the Hub-only delegation protocol. */
import assert from 'node:assert/strict';
import { existsSync, readFileSync, writeFileSync, mkdirSync, symlinkSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { afterEach, it } from 'node:test';
import { parse, stringify } from 'yaml';
import { startTestHub } from './helpers/hub.js';
import { connectWorker, until, upgradeStatus } from './helpers/worker.js';
import { sessionDir } from '../src/launch/config-render.ts';
import { createHub } from '../src/hub.ts';
import { hubRoot } from '../src/config.ts';
import { createLogger } from '../src/log.ts';
import { removeChildDirectory, readPrivate, writePrivate } from '../src/subagents/storage.ts';
const fixture = join(import.meta.dirname, 'fixtures', 'subagent-worker.js');
const hubs = [];
afterEach(async () => { for (const hub of hubs.splice(0)) await hub.stop(); });
async function setup(overrides = {}) {
    const ctx = await startTestHub({ worker: { bin: fixture, stopTimeoutMs: 50, sigtermGraceMs: 50, sigkillGraceMs: 1000 }, ...overrides });
    hubs.push(ctx.hub);
    const parent = ctx.hub.registry.create('parent');
    const result = await ctx.hub.supervisor.start(parent);
    assert.equal(result.ok, true, result.error);
    await until(() => parent.workerCapabilities?.names.includes('session-history'));
    parent.connection.sendSignal({ type: 'signal', data: { operation: 'test_active' } });
    await until(() => parent.activeRunId === 'parent-run');
    return { ...ctx, parent };
}
let count = 0;
async function rpc(ctx, route, args = {}, fields = {}, caller = ctx.parent) {
    const url = new URL(ctx.hub.supervisor.endpointsFor(caller.id, caller.token).tools);
    url.pathname += `/${route}`;
    const peer = await connectWorker(url.href);
    peer.send({ type: 'tool_request', data: { worker_id: caller.identity.workerId,
        session_id: caller.id, run_id: caller.activeRunId, request_id: `rpc-${++count}`, arguments: args, ...fields } });
    const response = (await peer.waitFor(value => value.type === 'tool_response')).data;
    await peer.waitForClose();
    return response;
}
async function fork(ctx, caller = ctx.parent) {
    const response = await rpc(ctx, 'subagent/clean-fork', {}, {}, caller);
    assert.equal(response.status, 'succeeded', JSON.stringify(response));
    const child = ctx.hub.registry.require(response.result.subagent_id);
    await until(() => child.subagent.lifecycle === 'ready');
    return child;
}
async function send(ctx, child, operation = 'message', extra = {}) {
    return rpc(ctx, 'subagent/send', { subagent_id: child.id, operation,
        ...(operation === 'message' ? { content: [{ type: 'text', modality: 'text', raw: 'child task' }] } : {}), ...extra });
}
it('clean-forks startup settings with independent identity and empty persistence', async () => {
    const ctx = await setup();
    const path = join(sessionDir(ctx.config, ctx.parent.id), 'config', 'config.yaml');
    const original = parse(readFileSync(path, 'utf8'));
    // Editing a live parent's file/defaults must not change its captured startup.
    const edited = structuredClone(original); edited.providers.deepseek.model = 'changed-later';
    writeFileSync(path, stringify(edited));
    ctx.config.providerProfiles.deepseek.model = 'library-changed';
    mkdirSync(join(sessionDir(ctx.config, ctx.parent.id), 'state'), { recursive: true });
    writeFileSync(join(sessionDir(ctx.config, ctx.parent.id), 'state', 'state.json'), 'parent history');
    const child = await fork(ctx);
    const root = sessionDir(ctx.config, child.id);
    assert.equal(root, join(ctx.config.dataDir, 'subagents', child.id));
    const actual = parse(readFileSync(join(root, 'config', 'config.yaml'), 'utf8'));
    assert.equal(actual.providers.deepseek.model, original.providers.deepseek.model);
    assert.equal(actual.persistence.directory, root);
    assert.equal(actual.persistence.restore, 'if_present');
    assert.match(actual.hub_remote_call.endpoint, new RegExp(`/agent/${child.id}/tools`));
    assert.match(actual.security.confirmation.endpoint, new RegExp(`/agent/${child.id}/confirm`));
    assert.notEqual(child.token, ctx.parent.token);
    assert.notEqual(child.lifecycleId, ctx.parent.lifecycleId);
    assert.equal(existsSync(join(root, 'state', 'state.json')), false);
    assert.equal(existsSync(join(root, 'events.jsonl')), false);
    assert.equal(ctx.hub.transcripts.transcripts.has(child.id), false);
    assert.equal(child.lastEvent, null);
    assert.deepEqual(child.latest, { status: null, options: null, run_finished: null });
});
it('deduplicates forks and payload writes and rejects reused identities/argument conflicts', async () => {
    const ctx = await setup();
    const fields = { request_id: 'fixed-create' };
    const [first, second] = await Promise.all([rpc(ctx, 'subagent/clean-fork', {}, fields), rpc(ctx, 'subagent/clean-fork', {}, fields)]);
    assert.equal(first.result.subagent_id, second.result.subagent_id);
    assert.equal(ctx.hub.subagents.children.size, 1);
    const child = ctx.hub.registry.require(first.result.subagent_id);
    await until(() => child.subagent.lifecycle === 'ready');
    const args = { subagent_id: child.id, operation: 'message', content: [{ type: 'text', modality: 'text', raw: 'only once' }] };
    const one = await rpc(ctx, 'subagent/send', args, { request_id: 'fixed-send' });
    const two = await rpc(ctx, 'subagent/send', { content: args.content, operation: args.operation,
        subagent_id: args.subagent_id }, { request_id: 'fixed-send' });
    assert.equal(one.result.request_id, two.result.request_id);
    const conflict = await rpc(ctx, 'subagent/send', { ...args, content: [{ type: 'text', modality: 'text', raw: 'different' }] }, { request_id: 'fixed-send' });
    assert.equal(conflict.error.code, 'request_conflict');
    const wrongWorker = await rpc(ctx, 'subagent/receive', {}, { worker_id: 'stale-worker' });
    assert.equal(wrongWorker.error.code, 'unauthorized');
    await until(() => ctx.hub.subagents.children.get(child.id).conversation.value.turns.length === 1);
});
it('supports send/continue/compact/receive while excluding tools, reasoning and raw events', async () => {
    const ctx = await setup();
    const child = await fork(ctx);
    const response = await send(ctx, child);
    assert.equal(response.result.state, 'sent');
    const record = ctx.hub.subagents.children.get(child.id);
    await until(() => !record.conversation.value.stale && record.conversation.value.turns[0]?.steps.length === 1);
    const received = await rpc(ctx, 'subagent/receive', { subagent_id: child.id });
    assert.equal(received.result.requests[0].state, 'finished');
    assert.equal(received.result.conversation.turns[0].user[0].raw, 'child task');
    assert.equal(received.result.conversation.turns[0].steps[0].content[0].raw, 'fixture answer');
    assert.doesNotMatch(JSON.stringify(received), /SECRET_|reasoning|tool_calls|invokes|"raw":\{/);
    assert.doesNotMatch(readFileSync(join(sessionDir(ctx.config, child.id), 'conversation.json'), 'utf8'), /SECRET_|reasoning|tool_calls/);
    await send(ctx, child, 'continue');
    await until(() => !record.conversation.value.stale && record.conversation.value.turns[0]?.steps.length === 2);
    assert.equal(record.conversation.value.turns.length, 1);
    await send(ctx, child, 'compact');
    await until(() => !record.conversation.value.stale && record.conversation.value.turns.length === 0);
    const after = await rpc(ctx, 'subagent/receive', { subagent_id: child.id });
    assert.equal(after.result.requests.at(-1).summary, 'fixture summary');
    assert.equal(ctx.hub.transcripts.transcripts.has(child.id), false);
});
it('enforces direct-parent scope, operator-only policy and limits before reservations', async () => {
    const ctx = await setup({ subagents: { maxChildren: 1, maxDepth: 2 } });
    const child = await fork(ctx);
    assert.equal((await rpc(ctx, 'subagent/clean-fork')).error.code, 'limit_exceeded');
    assert.equal((await send(ctx, child, 'continue', { options: { confirmation: { mode: 'approve' } } })).error.code, 'policy_forbidden');
    child.connection.sendSignal({ type: 'signal', data: { operation: 'test_active' } });
    await until(() => child.activeRunId === 'parent-run');
    const grandchild = await fork(ctx, child);
    assert.equal((await rpc(ctx, 'subagent/receive', { subagent_id: grandchild.id })).error.code, 'unauthorized');
    grandchild.connection.sendSignal({ type: 'signal', data: { operation: 'test_active' } });
    await until(() => grandchild.activeRunId === 'parent-run');
    assert.equal((await rpc(ctx, 'subagent/clean-fork', {}, {}, grandchild)).error.code, 'limit_exceeded');
    const roots = [child, grandchild].map(session => sessionDir(ctx.config, session.id));
    assert.equal((await ctx.hub.supervisor.stop(ctx.parent)).ok, true);
    for (const root of roots) assert.equal(existsSync(root), false);
    for (const session of [child, grandchild]) {
        assert.equal(session.subagent.lifecycle, 'stopped');
        assert.equal(session.process.logs.size, 0);
        assert.equal(session.process.args.length, 0);
    }
});
it('does not clean up on worker disconnect and refreshes primary dialogue after reconnect', async () => {
    const ctx = await setup();
    const child = await fork(ctx);
    await send(ctx, child);
    const record = ctx.hub.subagents.children.get(child.id);
    await until(() => !record.conversation.value.stale && record.conversation.value.turns.length === 1);
    child.connection.sendSignal({ type: 'signal', data: { operation: 'test_disconnect' } });
    await until(() => !child.connected);
    assert.equal(existsSync(sessionDir(ctx.config, child.id)), true);
    await until(() => child.connected && !record.conversation.value.stale);
    assert.equal(record.conversation.value.turns.length, 1);
    assert.equal(record.conversation.value.incomplete, false);
});
it('stops descendants on a parent process crash and reports terminal state without restoring directories', async () => {
    const ctx = await setup();
    const child = await fork(ctx);
    ctx.parent.connection.sendSignal({ type: 'signal', data: { operation: 'test_crash' } });
    await until(() => child.subagent.lifecycle === 'stopped');
    assert.equal(existsSync(sessionDir(ctx.config, child.id)), false);
});
it('blocks panel conversation/control/configuration APIs while retaining operator policy', async () => {
    const ctx = await setup();
    const child = await fork(ctx);
    for (const [method, suffix] of [['GET', 'events'], ['GET', 'snapshot'], ['GET', 'logs'], ['POST', 'start'], ['POST', 'stop'], ['POST', 'restart'], ['POST', 'configurations']]) {
        const result = await fetch(`${ctx.base}/api/sessions/${child.id}/${suffix}`, { method });
        assert.equal(result.status, 403, `${method} ${suffix}`);
    }
    const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
    await panel.waitFor(message => message.type === 'welcome');
    for (const command of [{ type: 'subscribe' }, { type: 'input', operation: 'continue' }, { type: 'signal', operation: 'shutdown' }, { type: 'history' }, { type: 'worker', action: 'restart' }, { type: 'status_snapshot' }]) {
        panel.send({ ...command, session: child.id });
        const error = await panel.waitFor(message => message.type === 'error' && message.request?.type === command.type);
        assert.equal(error.error, 'headless_restricted');
    }
    const changed = await fetch(`${ctx.base}/api/sessions/${child.id}/subagent-policy`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ policy: 'deny' }) });
    assert.equal(changed.status, 200);
    assert.equal(child.subagent.policy, 'deny');
    panel.close();
});
it('reserves generated IDs from manual creation and rejects symlink cleanup escapes', async () => {
    const ctx = await setup();
    const id = 'subagent-12345678-1234-4123-8123-123456789abc';
    const result = await fetch(`${ctx.base}/api/sessions`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ session: id }) });
    assert.equal(result.status, 400);
    const external = join(ctx.config.dataDir, 'outside');
    mkdirSync(external); writeFileSync(join(external, 'keep'), 'keep');
    mkdirSync(join(ctx.config.dataDir, 'subagents'), { recursive: true });
    symlinkSync(external, sessionDir(ctx.config, id));
    assert.throws(() => removeChildDirectory(ctx.config.dataDir, id), /unsafe directory/);
    assert.equal(readFileSync(join(external, 'keep'), 'utf8'), 'keep');
});
it('applies verified policies and leaves older ask prompts actionable after a change', async () => {
    const ctx = await setup({ limits: { confirmIdentityHoldMs: 50 } });
    const child = await fork(ctx);
    child.connection.sendSignal({ type: 'signal', data: { operation: 'test_active' } });
    await until(() => child.activeRunId === 'parent-run');
    async function confirm(id, worker = child.identity.workerId) {
        const peer = await connectWorker(ctx.hub.supervisor.endpointsFor(child.id, child.token).confirm);
        peer.send({ type: 'confirmation_request', data: { worker_id: worker, session_id: child.id,
            run_id: child.activeRunId, confirmation_id: id, call: { name: 'run_command', arguments: {} } } });
        return peer;
    }
    const ask = await confirm('old-ask');
    await until(() => child.prompts.size === 1);
    const old = child.prompts.get('old-ask');
    ctx.hub.subagents.setPolicy(child, 'deny');
    assert.equal(old.state, 'awaiting-decision');
    const denied = await confirm('new-deny');
    assert.equal((await denied.waitFor(m => m.type === 'confirmation_response')).data.decision, 'denied');
    await denied.close();
    assert.equal(old.decide('approved').ok, true);
    assert.equal((await ask.waitFor(m => m.type === 'confirmation_response')).data.decision, 'approved');
    await ask.close();
    ctx.hub.subagents.setPolicy(child, 'approve');
    for (const [id, worker, decision] of [['approve', child.identity.workerId, 'approved'], ['stale', 'obsolete', 'denied']]) {
        const peer = await confirm(id, worker);
        assert.equal((await peer.waitFor(m => m.type === 'confirmation_response')).data.decision, decision);
        await peer.close();
    }
});
it('freezes sends and forks immediately when a family starts stopping', async () => {
    const ctx = await setup();
    const child = await fork(ctx);
    const stopping = ctx.hub.supervisor.stop(ctx.parent);
    assert.equal(ctx.parent.closing, true); assert.equal(child.closing, true);
    assert.equal((await rpc(ctx, 'subagent/clean-fork')).error.code, 'unauthorized');
    assert.equal((await send(ctx, child, 'continue')).error.code, 'unauthorized');
    assert.equal((await stopping).ok, true);
});
it('cleans other siblings after a stop failure and waits for delayed owned IO', async () => {
    const ctx = await setup(); const one = await fork(ctx); const two = await fork(ctx);
    const original = ctx.hub.supervisor.stopProcess.bind(ctx.hub.supervisor);
    let failing = true;
    let release;
    two.process.ownedIo = new Promise(resolve => { release = resolve; });
    ctx.hub.supervisor.stopProcess = (session, options) => session === one && failing
        ? Promise.reject(new Error('injected stop failure')) : original(session, options);
    const stopped = ctx.hub.supervisor.stop(ctx.parent);
    await until(() => ['exited', 'failed'].includes(two.process.state));
    assert.equal(existsSync(sessionDir(ctx.config, two.id)), true);
    release();
    assert.equal((await stopped).ok, false);
    assert.equal(one.subagent.lifecycle, 'cleanup-pending');
    assert.equal(existsSync(sessionDir(ctx.config, one.id)), true);
    assert.equal(existsSync(sessionDir(ctx.config, two.id)), false);
    failing = false;
    assert.equal((await ctx.hub.supervisor.stop(one)).ok, true);
});
it('never retransmits an uncertain send after durable intent', async () => {
    const ctx = await setup(); const child = await fork(ctx);
    const previous = child.connection.sendPayload.bind(child.connection);
    child.connection.sendPayload = () => { throw new Error('injected transport failure'); };
    const args = { subagent_id: child.id, operation: 'continue' };
    const failure = await rpc(ctx, 'subagent/send', args, { request_id: 'lost-send' });
    assert.equal(failure.error.code, 'delivery_unknown');
    child.connection.sendPayload = previous;
    const replay = await rpc(ctx, 'subagent/send', args, { request_id: 'lost-send' });
    assert.equal(replay.result.state, 'unknown'); assert.equal(replay.result.replayed, true);
    assert.equal(child.requests.size, 1);
    const stored = JSON.parse(readFileSync(join(sessionDir(ctx.config, child.id), 'operations.json'), 'utf8'));
    assert.equal(stored.requests[0].state, 'unknown');
});
it('retains an unverified spawn reservation instead of deleting potentially live data', async () => {
    const ctx = await setup();
    const id = 'subagent-12345678-1234-4123-8123-123456789abd';
    const root = sessionDir(ctx.config, id);
    writePrivate(join(root, 'metadata.json'), { id, kind: 'headless', token: 'private-token',
        cascading_parent: { session_id: ctx.parent.id, lifecycle_id: ctx.parent.lifecycleId, worker_id: ctx.parent.identity.workerId },
        lifecycle_id: 'not-published', process: null, subagent: { policy: 'ask', lifecycle: 'starting' } }, 512 * 1024);
    await ctx.hub.subagents.restore();
    assert.equal(ctx.hub.registry.require(id).subagent.lifecycle, 'cleanup-pending');
    assert.equal(existsSync(root), true);
});
it('rejects unsupported daemonizing parents without leaking quota', async () => {
    const ctx = await setup();
    const launch = join(sessionDir(ctx.config, ctx.parent.id), 'config', 'startup-launch.jsonc');
    const document = JSON.parse(readFileSync(launch, 'utf8')); document.launcher.pidFile = '/tmp/detached.pid';
    writeFileSync(launch, JSON.stringify(document));
    assert.equal((await rpc(ctx, 'subagent/clean-fork')).error.code, 'unsupported_launch');
    assert.equal(ctx.hub.subagents.children.size, 0);
});
it('restores surviving families after an actual Hub process crash and then cascades stop', { timeout: 15000 }, async t => {
    const { spawn } = await import('node:child_process');
    const { createServer } = await import('node:net');
    const { testConfig } = await import('./helpers/hub.js');
    const freePort = async () => {
        const server = createServer(); await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
        const port = server.address().port; await new Promise(resolve => server.close(resolve)); return port;
    };
    const config = testConfig({ listen: { host: '127.0.0.1', port: await freePort() },
        toolRequests: { port: await freePort() }, worker: { bin: fixture, stopTimeoutMs: 50, sigtermGraceMs: 50 } });
    const file = join(config.dataDir, 'hub-test.json'); writeFileSync(file, JSON.stringify(config));
    const base = `http://127.0.0.1:${config.listen.port}`;
    let process;
    let output = '';
    async function launch() {
        process = spawn(globalThis.process.execPath, [join(hubRoot, 'bin', 'simplex-hub.ts'), '--config', file], { stdio: ['ignore', 'pipe', 'pipe'] });
        process.stdout.on('data', chunk => { output += chunk; }); process.stderr.on('data', chunk => { output += chunk; });
        await until(async () => { try { return (await fetch(`${base}/api/meta`)).ok; } catch { return false; } }, { timeout: 5000 });
    }
    async function api(path, body) {
        const response = await fetch(base + path, body === undefined ? {} : {
            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
        return response.json();
    }
    let childId;
    t.after(async () => {
        try { await api('/api/sessions/recovery-parent/stop', {}); } catch {}
        if (process && process.exitCode === null) { process.kill('SIGTERM'); await new Promise(resolve => process.once('exit', resolve)); }
    });
    await launch();
    await api('/api/sessions', { session: 'recovery-parent',
        spec: { env: { SIMPLEX_FIXTURE_ACTIVE_SESSION: 'recovery-parent' } } });
    await api('/api/sessions/recovery-parent/start', {});
    await until(async () => (await api('/api/sessions/recovery-parent')).session.connected);
    const storedParent = JSON.parse(readFileSync(join(config.dataDir, 'hub.json'), 'utf8')).sessions[0];
    await until(async () => (await api('/api/sessions/recovery-parent')).session.last_run_id === 'parent-run');
    const identity = (await api('/api/sessions/recovery-parent')).session.identity.worker_id;
    const url = `ws://127.0.0.1:${config.toolRequests.port}/agent/recovery-parent/tools/subagent/clean-fork?token=${storedParent.token}`;
    const tools = await connectWorker(url);
    tools.send({ type: 'tool_request', data: { session_id: 'recovery-parent', worker_id: identity,
        run_id: 'parent-run', request_id: 'recovery-fork', arguments: {} } });
    const response = await tools.waitFor(value => value.type === 'tool_response');
    assert.equal(response.data.status, 'succeeded', JSON.stringify(response));
    childId = response.data.result.subagent_id;
    await tools.waitForClose();
    await until(async () => (await api(`/api/sessions/${childId}`)).session.subagent.lifecycle === 'ready');
    const task = await connectWorker(url.replace('subagent/clean-fork', 'subagent/send'));
    task.send({ type: 'tool_request', data: { session_id: 'recovery-parent', worker_id: identity,
        run_id: 'parent-run', request_id: 'recovery-send', arguments: { subagent_id: childId, operation: 'message',
            content: [{ type: 'text', modality: 'text', raw: 'Survive the Hub crash.' }] } } });
    assert.equal((await task.waitFor(value => value.type === 'tool_response')).data.status, 'succeeded');
    await task.waitForClose();
    const conversationPath = join(sessionDir(config, childId), 'conversation.json');
    await until(() => JSON.parse(readFileSync(conversationPath, 'utf8')).turns[0]?.steps.length);
    process.kill('SIGKILL'); await new Promise(resolve => process.once('exit', resolve));
    await launch();
    await until(async () => (await api(`/api/sessions/${childId}`)).session.connected, { timeout: 5000 });
    const recovered = (await api(`/api/sessions/${childId}`)).session;
    assert.equal(recovered.kind, 'headless'); assert.equal(recovered.subagent.policy, 'ask');
    assert.equal(recovered.subagent.parent, 'recovery-parent');
    assert.equal(existsSync(sessionDir(config, childId)), true);
    await until(() => !JSON.parse(readFileSync(conversationPath, 'utf8')).stale);
    const dialogue = JSON.parse(readFileSync(conversationPath, 'utf8'));
    assert.equal(dialogue.turns[0].user[0].raw, 'Survive the Hub crash.');
    assert.doesNotMatch(JSON.stringify(dialogue), /SECRET_TOOL|SECRET_ARG|SECRET_RESULT|SECRET_REASONING/);
    assert.equal((await api('/api/sessions/recovery-parent/stop', {})).ok, true, output);
    assert.equal(existsSync(sessionDir(config, childId)), false);
});

it('retires pending headless approvals immediately when shutdown starts', async () => {
    const ctx = await setup();
    const child = await fork(ctx);
    child.connection.sendSignal({ type: 'signal', data: { operation: 'test_active' } });
    await until(() => child.activeRunId === 'parent-run');
    const url = ctx.hub.supervisor.endpointsFor(child.id, child.token).confirm;
    const peer = await connectWorker(url);
    peer.send({ type: 'confirmation_request', data: { confirmation_id: 'stop-approval',
        worker_id: child.identity.workerId, session_id: child.id, run_id: child.activeRunId,
        deadline_ms: 10000, call: { id: 'call', name: 'run_command', arguments: { command: 'echo test' },
            security: 'require_confirm', type: 'serial_write' } } });
    await until(() => child.prompts.size === 1);
    const prompt = child.prompts.get('stop-approval');
    const stopped = ctx.hub.supervisor.stop(child);
    assert.equal(prompt.state, 'retired');
    assert.equal(prompt.decide('approved').ok, false);
    await peer.waitForClose();
    assert.equal((await stopped).ok, true);
});

it('does not start a fork when its RPC peer disconnects before live-run authorization', async () => {
    const ctx = await setup();
    ctx.parent.activeRunId = '';
    const url = new URL(ctx.hub.supervisor.endpointsFor(ctx.parent.id, ctx.parent.token).tools);
    url.pathname += '/subagent/clean-fork';
    const peer = await connectWorker(url.href);
    peer.send({ type: 'tool_request', data: { worker_id: ctx.parent.identity.workerId,
        session_id: ctx.parent.id, run_id: 'parent-run', request_id: 'aborted-before-commit', arguments: {} } });
    await peer.close();
    ctx.parent.connection.sendSignal({ type: 'signal', data: { operation: 'test_active' } });
    await until(() => ctx.parent.activeRunId === 'parent-run');
    await new Promise(resolve => setTimeout(resolve, 30));
    assert.equal(ctx.hub.subagents.children.size, 0);
});

it('releases quota only after cleanup and safely cleans failed automatic startup', async () => {
    const ctx = await setup({ subagents: { maxChildren: 1 } });
    const child = await fork(ctx);
    assert.equal((await rpc(ctx, 'subagent/clean-fork')).error.code, 'limit_exceeded');
    const stop = await send(ctx, child, 'stop');
    assert.equal(stop.result.state, 'stopping');
    await until(() => child.subagent.lifecycle === 'stopped');
    const original = ctx.hub.supervisor.start.bind(ctx.hub.supervisor);
    ctx.hub.supervisor.start = session => session.kind === 'headless'
        ? Promise.resolve({ ok: false, error: 'injected startup failure' }) : original(session);
    const failed = await rpc(ctx, 'subagent/clean-fork');
    assert.equal(failed.status, 'succeeded');
    const reserved = ctx.hub.registry.require(failed.result.subagent_id);
    await until(() => reserved.subagent.lifecycle === 'stopped');
    assert.equal(existsSync(sessionDir(ctx.config, reserved.id)), false);
    ctx.hub.supervisor.start = original;
    const replacement = await fork(ctx);
    assert.equal(replacement.subagent.lifecycle, 'ready');
});

it('returns cleanup-pending for an unjoined output fence and deletes only after a successful retry', { timeout: 10000 }, async () => {
    const ctx = await setup();
    const child = await fork(ctx);
    let release;
    child.process.ownedIo = new Promise(resolve => { release = resolve; });
    const result = await ctx.hub.supervisor.stop(child);
    assert.equal(result.ok, false);
    assert.equal(child.subagent.lifecycle, 'cleanup-pending');
    assert.equal(existsSync(sessionDir(ctx.config, child.id)), true);
    release();
    assert.equal((await ctx.hub.supervisor.stop(child)).ok, true);
    assert.equal(existsSync(sessionDir(ctx.config, child.id)), false);
});

it('cleans safe unspawned orphan/cycle reservations independently of directory order', async () => {
    const ctx = await setup();
    const ids = [
        'subagent-12345678-1234-4123-8123-123456789ab1',
        'subagent-12345678-1234-4123-8123-123456789ab2',
        'subagent-12345678-1234-4123-8123-123456789ab3',
    ];
    for (const [index, id] of ids.entries()) {
        writePrivate(join(sessionDir(ctx.config, id), 'metadata.json'), {
            id, kind: 'headless', token: `token-${index}`, lifecycle_id: `lifecycle-${index}`,
            cascading_parent: { session_id: index === 2 ? 'missing-parent' : ids[1 - index],
                lifecycle_id: `lifecycle-${1 - index}`, worker_id: 'missing-worker' },
            subagent: { lifecycle: 'preparing', policy: 'ask' }, process: null,
        }, 512 * 1024);
    }
    await ctx.hub.subagents.restore();
    for (const id of ids) assert.equal(existsSync(sessionDir(ctx.config, id)), false);
});
