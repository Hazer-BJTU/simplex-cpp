/**
 * @file end-to-end runs against the real `simplex_worker` binary.
 *
 * These are the only tests that exercise the C++ worker: everything else uses a
 * stand-in process. They need a built tree (`build/bin/simplex_worker` plus its
 * plugins and prompts) and are skipped without one. No API key is required —
 * the hub's offline mock provider supplies the model.
 *
 * Run with `npm run test:e2e` (or `SIMPLEX_WORKER_BIN=/path/to/simplex_worker`).
 */
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync } from 'node:fs';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, it } from 'node:test';
import { hubRoot } from '../../src/config.ts';
import { persistenceRoot, sessionDir } from '../../src/launch/config-render.ts';
import { WORKER_BIN, PROMPTS_DIR, e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';

const skip = e2eSkip;

/** Fetch JSON from a hub base URL. */
async function api(base, path, options = {}) {
    const response = await fetch(`${base}${path}`, {
        ...options,
        headers: { 'Content-Type': 'application/json', ...(options.headers ?? {}) },
        body: options.body === undefined ? undefined : JSON.stringify(options.body),
    });
    const text = await response.text();
    return { status: response.status, body: text ? JSON.parse(text) : null };
}

/** An unused loopback port. */
function freePort() {
    return new Promise((resolve, reject) => {
        const server = createServer();
        server.once('error', reject);
        server.listen(0, '127.0.0.1', () => {
            const { port } = server.address();
            server.close(() => resolve(port));
        });
    });
}

/** Start a hub as a child process and wait until it answers. */
async function startHubProcess({ port, dataDir, mock = true }) {
    const args = [
        join(hubRoot, 'bin', 'simplex-hub.ts'),
        '--listen', `127.0.0.1:${port}`,
        '--data-dir', dataDir,
        '--worker-bin', WORKER_BIN,
        '--prompts-dir', PROMPTS_DIR,
        '--log-level', 'info',
    ];
    if (mock) args.push('--mock');
    const child = spawn(process.execPath, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    child.stdout.on('data', (chunk) => { output += chunk; });
    child.stderr.on('data', (chunk) => { output += chunk; });
    const base = `http://127.0.0.1:${port}`;
    await until(async () => {
        try {
            const response = await fetch(`${base}/api/meta`);
            return response.ok;
        } catch {
            return false;
        }
    }, { timeout: 15000, label: 'hub to start' }).catch((error) => {
        throw new Error(`${error.message}\nhub output:\n${output}`);
    });
    return { child, base, output: () => output };
}

/** Wait for a hub started as a child to exit. */
function waitForExit(child, timeoutMs = 10000) {
    return Promise.race([
        new Promise((resolve) => child.once('exit', (code, signal) => resolve({ code, signal }))),
        new Promise((resolve) => {
            const timer = setTimeout(() => resolve(null), timeoutMs);
            timer.unref?.();
        }),
    ]);
}

describe('end to end with the real worker', { skip }, () => {
    it('runs a tool call, asks for confirmation, and completes', { timeout: 180000 }, async () => {
        const ctx = await startE2eHub();
        const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
        try {
            const created = await api(ctx.base, '/api/sessions', {
                method: 'POST',
                body: { session: 'e2e-live', spec: { provider: 'mock', model: 'mock-auto' } },
            });
            assert.equal(created.status, 201, JSON.stringify(created.body));

            const started = await api(ctx.base, '/api/sessions/e2e-live/start', { method: 'POST' });
            assert.equal(started.body.ok, true, started.body.error);

            const session = ctx.hub.registry.get('e2e-live');
            await until(() => session.connected && session.identity.state === 'live',
                { timeout: 60000, label: 'the worker to connect' });
            await until(() => session.latest.status !== null,
                { timeout: 30000, label: 'a status snapshot' });
            assert.equal(session.latest.status.data.active, false);

            panel.send({ v: 1, type: 'subscribe', session: 'e2e-live' });
            await panel.waitFor((message) => message.type === 'subscribed', { timeout: 10000 });
            panel.send({
                v: 1,
                type: 'input',
                session: 'e2e-live',
                content: [{ type: 'text', raw: 'Run the fixture command' }],
            });
            const accepted = await panel.waitFor((message) => message.type === 'accepted',
                { timeout: 10000, label: 'input acceptance' });
            assert.equal(accepted.action, 'input');

            // `run_command` requires confirmation, so the worker opens a second
            // connection and the hub turns it into a panel prompt.
            const promptMessage = await panel.waitFor(
                (message) => message.type === 'confirmation' && message.open === true,
                { timeout: 60000, label: 'a confirmation prompt' });
            const prompt = promptMessage.confirmation;
            assert.equal(prompt.call.name, 'run_command');
            assert.equal(prompt.verified, true);
            assert.match(prompt.call.arguments.command, /mock stdout/);

            panel.send({
                v: 1, type: 'confirmation', session: 'e2e-live',
                confirmation_id: prompt.confirmation_id,
                decision: 'approved', reason: 'end-to-end test',
            });
            const settled = await panel.waitFor(
                (message) => message.type === 'confirmation' && message.open === false,
                { timeout: 60000, label: 'the prompt to settle' });
            assert.equal(settled.outcome.phase, 'decided');

            const finished = await panel.waitFor(
                (message) => message.type === 'event' && message.envelope.event === 'run_finished',
                { timeout: 90000, label: 'run_finished' });
            assert.equal(finished.envelope.data.status, 'completed',
                JSON.stringify(finished.envelope.data));
            assert.equal(finished.envelope.data.durable, true);

            // The tool really ran: `run_command` inherits the worker's working
            // directory, which the hub set to the session directory.
            const marker = join(sessionDir(ctx.config, 'e2e-live'), 'mock-tool-marker.txt');
            assert.ok(existsSync(marker), `missing tool marker at ${marker}`);

            const transcript = ctx.hub.transcripts.get('e2e-live').toArray();
            const results = transcript.filter((envelope) => envelope.event === 'tool_results');
            assert.ok(results.length >= 1, 'no tool_results event');
            const [result] = results[0].data;
            assert.equal(result.type, 'invoke_return');
            assert.equal(result.role, 'tool');
            assert.match(result.invoke_return.output.raw, /mock stdout/);
            assert.match(result.invoke_return.output.raw, /mock stderr/);
            // A proposed call is not yet classified: the settled query carries
            // the host-resolved security and scheduling class.
            const proposed = transcript.find((envelope) => envelope.event === 'tool_calls').data[0];
            assert.equal(proposed.security, 'default_deny');
            assert.equal(result.invoke_return.query.security, 'require_confirm');
            assert.equal(result.invoke_return.query.type, 'serial_write');
            assert.equal(result.invoke_return.query.id, proposed.id);
            assert.ok(transcript.some((envelope) => envelope.event === 'model_response'));
            assert.ok(transcript.some(
                (envelope) => envelope.event === 'persisted' && envelope.data.boundary === 'before_tools'));

            // The worker owns the authoritative snapshot.
            const snapshotPath = join(persistenceRoot(ctx.config), 'e2e-live', 'state.json');
            await until(() => existsSync(snapshotPath), { timeout: 30000, label: 'state.json' });
            const snapshot = JSON.parse(readFileSync(snapshotPath, 'utf8'));
            assert.equal(snapshot.loop.status, 'completed');
            assert.equal(snapshot.loop.phase, 'ready');

            const stopped = await api(ctx.base, '/api/sessions/e2e-live/stop', { method: 'POST' });
            assert.equal(stopped.body.ok, true, JSON.stringify(stopped.body));
            assert.equal(session.process.exitCode, 0);
        } finally {
            await panel.close();
            await ctx.hub.stop();
        }
    });

    it('adopts a worker that outlives a crashed hub', { timeout: 180000 }, async () => {
        const port = await freePort();
        const dataDir = mkdtempSync(join(tmpdir(), 'simplex-hub-orphan-'));
        const first = await startHubProcess({ port, dataDir });
        let second = null;
        try {
            await api(first.base, '/api/sessions', {
                method: 'POST',
                body: { session: 'orphan', spec: { provider: 'mock', model: 'mock-text' } },
            });
            const started = await api(first.base, '/api/sessions/orphan/start', { method: 'POST' });
            assert.equal(started.body.ok, true, started.body.error);
            const workerPid = started.body.pid;

            await until(async () => {
                const described = await api(first.base, '/api/sessions/orphan');
                return described.body.session.connected;
            }, { timeout: 60000, label: 'the first hub to see the worker' });
            // Adoption is only possible when the process record reached disk.
            await until(() => existsSync(join(dataDir, 'hub.json')),
                { timeout: 10000, label: 'the hub state file' });
            const stored = JSON.parse(readFileSync(join(dataDir, 'hub.json'), 'utf8'));
            assert.equal(stored.sessions[0].process.pid, workerPid);
            assert.ok(stored.sessions[0].process.pid_start_time);

            // Simulate a hub crash: the process dies without stopping anything.
            first.child.kill('SIGKILL');
            await waitForExit(first.child);

            second = await startHubProcess({ port, dataDir });
            // The restored session must remember the worker's pid, prove it is
            // the same incarnation through /proc, and see it reconnect.
            const adopted = await until(async () => {
                const described = await api(second.base, '/api/sessions/orphan');
                const session = described.body.session;
                return session.process?.state === 'running' ? session : false;
            }, { timeout: 30000, label: 'the second hub to adopt the worker' });
            assert.equal(adopted.process.pid, workerPid);
            await until(async () => {
                const described = await api(second.base, '/api/sessions/orphan');
                return described.body.session.connected;
            }, { timeout: 60000, label: 'the worker to reconnect' });

            const stopped = await api(second.base, '/api/sessions/orphan/stop', { method: 'POST' });
            assert.equal(stopped.body.ok, true, JSON.stringify(stopped.body));
            assert.equal(stopped.body.how, 'shutdown-signal');
        } finally {
            second?.child.kill('SIGTERM');
            if (second) await waitForExit(second.child);
            if (first.child.exitCode === null && first.child.signalCode === null) {
                first.child.kill('SIGKILL');
            }
        }
    });
});
