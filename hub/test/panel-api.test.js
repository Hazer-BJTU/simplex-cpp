/**
 * @file the panel API: REST management, the versioned panel WebSocket, replay,
 * request tracking, worker actions, and the trust boundary around all of it.
 */
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { after, before, describe, it } from 'node:test';
import { persistenceRoot } from '../src/launch/config-render.ts';
import { IDENTITY } from '../src/state/registry.ts';
import { connectWorker, upgradeStatus, until, workerEvent } from './helpers/worker.js';
import { startTestHub } from './helpers/hub.js';

const FIXTURE = join(import.meta.dirname, 'fixtures', 'fake-worker.js');

describe('panel API', () => {
    let ctx;
    const sockets = [];

    before(async () => {
        ctx = await startTestHub({
            worker: { bin: FIXTURE, stopTimeoutMs: 200, sigtermGraceMs: 200, sigkillGraceMs: 1000 },
        });
    });

    after(async () => {
        for (const socket of sockets) await socket.close();
        await ctx.hub.stop();
    });

    /** Open a panel socket. */
    async function panel() {
        const socket = await connectWorker(`${ctx.wsBase}/panel/ws`);
        sockets.push(socket);
        await socket.waitFor((message) => message.type === 'welcome', { label: 'welcome' });
        return socket;
    }

    /** Fetch a JSON API path. */
    async function api(path, options = {}) {
        const response = await fetch(`${ctx.base}${path}`, {
            ...options,
            headers: { 'Content-Type': 'application/json', ...(options.headers ?? {}) },
            body: options.body === undefined ? undefined : JSON.stringify(options.body),
        });
        const text = await response.text();
        return { status: response.status, body: text ? JSON.parse(text) : null };
    }

    /** Connect a worker event socket that identifies itself. */
    async function identify(session, workerId = 'panel-worker') {
        const worker = await connectWorker(
            `${ctx.wsBase}/agent/${session.id}/events?token=${session.token}`);
        sockets.push(worker);
        worker.send(workerEvent({
            session: session.id, worker: workerId, sequence: 1, event: 'status', data: { active: false },
        }));
        await until(() => session.identity.state === IDENTITY.live, { label: 'live identity' });
        return worker;
    }

    it('welcomes a panel client with metadata and the session list', async () => {
        const session = ctx.hub.registry.create('welcome-session');
        const socket = await panel();
        const welcome = socket.messages.find((message) => message.type === 'welcome');
        assert.equal(welcome.v, 1);
        assert.equal(welcome.hub.name, 'simplex-hub');
        assert.equal(welcome.hub.protocol.version, 1);
        assert.ok(welcome.hub.capabilities.includes('transcript-replay'));
        assert.ok(welcome.sessions.some((entry) => entry.session_id === session.id));
    });

    it('forwards history queries without retaining their large replies', async () => {
        const session = ctx.hub.registry.create('history-session');
        const worker = await identify(session, 'history-worker');
        const socket = await panel();
        socket.send({ v: 1, type: 'subscribe', session: session.id });
        await socket.waitFor((message) => message.type === 'subscribed'
            && message.session.session_id === session.id);
        socket.send({ v: 1, type: 'history', session: session.id,
            request_id: 'history-1', start: 0, limit: 10 });
        const query = await worker.waitFor((message) => message.type === 'payload'
            && message.data.operation === 'history');
        assert.equal(query.data.request_id, 'history-1');
        worker.send(workerEvent({ session: session.id, worker: 'history-worker',
            sequence: 2, event: 'history', data: { request_id: 'history-1',
                start: 0, next: 1, total: 1, turns: [{ index: 0, user: [],
                    steps: [], omitted_steps: 0 }] } }));
        const reply = await socket.waitFor((message) => message.type === 'event'
            && message.envelope.event === 'history');
        assert.equal(reply.envelope.data.turns.length, 1);
        const marker = ctx.hub.transcripts.get(session.id).toArray()
            .find((entry) => entry.event === 'history');
        assert.ok(marker);
        assert.equal(marker.data.turns, undefined);
        assert.equal(marker.transient_history, true);
        const laterPanel = await panel();
        laterPanel.send({ v: 1, type: 'subscribe', session: session.id, since: 0 });
        const replay = await laterPanel.waitFor((message) => message.type === 'subscribed'
            && message.session.session_id === session.id);
        assert.ok(replay.transcript.some((entry) => entry.event === 'history'
            && entry.transient_history && !entry.data.turns));
    });

    it('creates, lists, reads, and deletes sessions over REST', async () => {
        const created = await api('/api/sessions', {
            method: 'POST',
            body: { session: 'rest-created', spec: { provider: 'mock', threads: 2 } },
        });
        assert.equal(created.status, 201);
        assert.equal(created.body.session.session_id, 'rest-created');
        assert.equal(created.body.session.spec.provider, 'mock');

        const duplicate = await api('/api/sessions', { method: 'POST', body: { session: 'rest-created' } });
        assert.equal(duplicate.status, 409);
        assert.equal(duplicate.body.error, 'session_exists');

        const invalid = await api('/api/sessions', { method: 'POST', body: { session: '../etc' } });
        assert.equal(invalid.status, 400);
        assert.equal(invalid.body.error, 'invalid_session');

        const list = await api('/api/sessions');
        assert.equal(list.status, 200);
        assert.ok(list.body.sessions.some((entry) => entry.session_id === 'rest-created'));

        const read = await api('/api/sessions/rest-created');
        assert.equal(read.status, 200);
        assert.equal(read.body.session.session_id, 'rest-created');

        const missing = await api('/api/sessions/nope');
        assert.equal(missing.status, 404);

        const removed = await api('/api/sessions/rest-created', { method: 'DELETE' });
        assert.equal(removed.status, 200);
        assert.equal(removed.body.removed, 'rest-created');
        assert.equal(ctx.hub.registry.get('rest-created'), undefined);
    });

    it('refuses to delete a session that still has a worker', async () => {
        const session = ctx.hub.registry.create('busy-session');
        await identify(session);
        const refused = await api('/api/sessions/busy-session', { method: 'DELETE' });
        assert.equal(refused.status, 409);
        assert.equal(refused.body.error, 'session_busy');
    });

    it('replays the transcript from a hub sequence number', async () => {
        const session = ctx.hub.registry.create('replay-session');
        const socket = await panel();
        socket.send({ v: 1, type: 'subscribe', session: session.id });
        const subscribed = await socket.waitFor(
            (message) => message.type === 'subscribed' && message.session.session_id === session.id);
        assert.deepEqual(subscribed.transcript, []);
        assert.equal(subscribed.latest, 0);

        const worker = await identify(session, 'replay-worker');
        for (let sequence = 2; sequence <= 4; sequence += 1) {
            worker.send(workerEvent({
                session: session.id,
                worker: 'replay-worker',
                sequence,
                event: 'run_started',
                runId: 'run-9',
            }));
        }
        await until(() => ctx.hub.transcripts.get(session.id).size === 4, { label: 'four events' });
        const first = await socket.waitFor((message) => message.type === 'event');
        assert.equal(first.session, session.id);
        assert.equal(first.hub_seq, 1);
        assert.equal(first.envelope.event, 'status');
        assert.equal(first.envelope.raw.type, 'event');

        const selected = await api(`/api/sessions/${session.id}/events?since=2`);
        assert.equal(selected.status, 200);
        assert.deepEqual(selected.body.events.map((event) => event.hub_sequence), [3, 4]);
        assert.equal(selected.body.latest, 4);

        // A later subscriber resumes from where it stopped.
        const second = await panel();
        second.send({ v: 1, type: 'subscribe', session: session.id, since: 3 });
        const resumed = await second.waitFor(
            (message) => message.type === 'subscribed' && message.session.session_id === session.id);
        assert.deepEqual(resumed.transcript.map((event) => event.hub_sequence), [4]);
    });

    it('sends an input and tracks its outcome from admission', async () => {
        const session = ctx.hub.registry.create('input-session');
        const worker = await identify(session, 'input-worker');
        const socket = await panel();
        socket.send({ v: 1, type: 'subscribe', session: session.id });
        await socket.waitFor((message) => message.type === 'subscribed');

        socket.send({
            v: 1,
            type: 'input',
            session: session.id,
            content: [{ type: 'text', raw: 'hello' }],
            options: { confirmation: { mode: 'deny' } },
        });
        const accepted = await socket.waitFor((message) => message.type === 'accepted');
        assert.equal(accepted.action, 'input');
        assert.ok(accepted.request_id.length > 0);

        const payload = await worker.waitFor((message) => message.type === 'payload');
        assert.equal(payload.data.operation, 'message');
        assert.deepEqual(payload.data.content, [{ type: 'text', raw: 'hello' }]);
        assert.deepEqual(payload.data.options, { confirmation: { mode: 'deny' } });
        assert.equal(payload.data.request_id, accepted.request_id);

        const sent = await socket.waitFor(
            (message) => message.type === 'request' && message.request.state === 'sent');
        assert.equal(sent.request.request_id, accepted.request_id);

        worker.send(workerEvent({
            session: session.id,
            worker: 'input-worker',
            sequence: 2,
            event: 'input_admitted',
            requestId: accepted.request_id,
            runId: 'run-input',
        }));
        const admitted = await socket.waitFor(
            (message) => message.type === 'request' && message.request.state === 'admitted');
        assert.equal(admitted.request.request_id, accepted.request_id);
        assert.equal(session.lastRunId, 'run-input');
    });

    it('reports an input rejection with the worker diagnostic', async () => {
        const session = ctx.hub.registry.create('reject-session');
        const worker = await identify(session, 'reject-worker');
        const socket = await panel();
        socket.send({ v: 1, type: 'subscribe', session: session.id });
        await socket.waitFor((message) => message.type === 'subscribed');
        socket.send({ v: 1, type: 'input', session: session.id, content: [{ type: 'text', raw: 'x' }] });
        const accepted = await socket.waitFor((message) => message.type === 'accepted');
        worker.send(workerEvent({
            session: session.id,
            worker: 'reject-worker',
            sequence: 2,
            event: 'input_rejected',
            data: { request_id: accepted.request_id, message: 'duplicate request_id' },
        }));
        const rejected = await socket.waitFor(
            (message) => message.type === 'request' && message.request.state === 'rejected');
        assert.equal(rejected.request.detail, 'duplicate request_id');
    });

    it('marks an in-flight input unknown when the worker disappears', async () => {
        const session = ctx.hub.registry.create('unknown-session');
        const worker = await identify(session, 'unknown-worker');
        const socket = await panel();
        socket.send({ v: 1, type: 'subscribe', session: session.id });
        await socket.waitFor((message) => message.type === 'subscribed');
        socket.send({ v: 1, type: 'input', session: session.id, content: [{ type: 'text', raw: 'x' }] });
        await socket.waitFor((message) => message.type === 'accepted');
        await worker.close();
        await until(() => session.describeRequests().some((entry) => entry.state === 'unknown'),
            { label: 'unknown request outcome' });
    });

    it('refuses input validation mistakes and an offline worker', async () => {
        const session = ctx.hub.registry.create('offline-session');
        const socket = await panel();
        socket.send({ v: 1, type: 'input', session: session.id, content: [{ type: 'text', raw: 'x' }] });
        const offline = await socket.waitFor((message) => message.type === 'error');
        assert.equal(offline.error, 'input_not_sent');
        assert.match(offline.message, /not connected/);

        await identify(session, 'offline-worker');
        socket.send({ v: 1, type: 'input', session: session.id, content: [] });
        const invalid = await socket.waitFor(
            (message) => message.type === 'error' && message.error === 'input_not_sent'
                && /nonempty array/.test(message.message));
        assert.ok(invalid);
    });

    it('targets cancellation at the most recent run', async () => {
        const session = ctx.hub.registry.create('cancel-session');
        const worker = await identify(session, 'cancel-worker');
        const socket = await panel();
        worker.send(workerEvent({
            session: session.id, worker: 'cancel-worker', sequence: 2,
            event: 'run_started', runId: 'run-cancel',
        }));
        await until(() => session.lastRunId === 'run-cancel');
        socket.send({ v: 1, type: 'signal', session: session.id, operation: 'cancel' });
        const signal = await worker.waitFor(
            (message) => message.type === 'signal' && message.data.operation === 'cancel');
        assert.equal(signal.data.run_id, 'run-cancel');

        socket.send({ v: 1, type: 'signal', session: session.id, operation: 'status' });
        await worker.waitFor(
            (message) => message.type === 'signal' && message.data.operation === 'status');
        await socket.waitFor((message) => message.type === 'accepted' && message.operation === 'status');
    });

    it('answers a confirmation from the panel', async () => {
        const session = ctx.hub.registry.create('confirm-session');
        await identify(session, 'confirm-worker');
        const socket = await panel();
        socket.send({ v: 1, type: 'subscribe', session: session.id });
        await socket.waitFor((message) => message.type === 'subscribed');

        const confirmation = await connectWorker(
            `${ctx.wsBase}/agent/${session.id}/confirm?token=${session.token}`);
        sockets.push(confirmation);
        confirmation.send({
            type: 'confirmation_request',
            data: {
                worker_id: 'confirm-worker',
                session_id: session.id,
                run_id: 'run-1',
                confirmation_id: 'c-panel',
                call: { name: 'run_command', arguments: { command: 'rm -rf /' } },
            },
        });
        const prompt = await socket.waitFor(
            (message) => message.type === 'confirmation' && message.open === true);
        assert.equal(prompt.confirmation.confirmation_id, 'c-panel');
        assert.equal(prompt.confirmation.call.name, 'run_command');

        socket.send({
            v: 1, type: 'confirmation', session: session.id,
            confirmation_id: 'c-panel', decision: 'denied', reason: 'not on my watch',
        });
        const response = await confirmation.waitFor(
            (message) => message.type === 'confirmation_response');
        assert.equal(response.data.decision, 'denied');
        assert.equal(response.data.reason, 'not on my watch');
        assert.equal(response.data.worker_id, 'confirm-worker');
        const closed = await socket.waitFor(
            (message) => message.type === 'confirmation' && message.open === false);
        assert.equal(closed.outcome.phase, 'decided');
        await confirmation.close();
    });

    it('starts, reports, and stops a worker through the API', async () => {
        const created = await api('/api/sessions', {
            method: 'POST',
            body: { session: 'supervised-rest', spec: { threads: 1 } },
        });
        assert.equal(created.status, 201);
        const socket = await panel();
        socket.send({ v: 1, type: 'subscribe', session: 'supervised-rest' });
        await socket.waitFor((message) => message.type === 'subscribed');

        const started = await api('/api/sessions/supervised-rest/start', { method: 'POST' });
        assert.equal(started.status, 200, JSON.stringify(started.body));
        assert.equal(started.body.ok, true);
        const processMessage = await socket.waitFor(
            (message) => message.type === 'process' && message.process?.state === 'running',
            { label: 'running process', timeout: 5000 });
        assert.ok(Number.isInteger(processMessage.process.pid));

        await until(() => ctx.hub.registry.get('supervised-rest').connected,
            { label: 'worker connection', timeout: 5000 });
        // Worker output reaches the hub through a pipe, so wait for the line
        // instead of assuming it arrived with the socket.
        await until(() => ctx.hub.supervisor.logs(ctx.hub.registry.get('supervised-rest'))
            .some((line) => line.includes('fixture: connected')), { label: 'worker output' });
        const logs = await api('/api/sessions/supervised-rest/logs?limit=50');
        assert.equal(logs.status, 200);
        assert.ok(logs.body.lines.some((line) => line.includes('fixture: connected')));
        assert.ok(logs.body.log_path.endsWith('worker.log'));

        const stopped = await api('/api/sessions/supervised-rest/stop', { method: 'POST' });
        assert.equal(stopped.body.ok, true);
        assert.equal(stopped.body.how, 'shutdown-signal');
        const exited = await socket.waitFor(
            (message) => message.type === 'process' && message.process?.state === 'exited');
        assert.equal(exited.process.exit_code, 0);
    });

    it('serves the worker snapshot read-only', async () => {
        const session = ctx.hub.registry.create('snapshot-session');
        const directory = join(persistenceRoot(ctx.config), session.id);
        mkdirSync(directory, { recursive: true });
        writeFileSync(join(directory, 'state.json'), JSON.stringify({ loop: { status: 'completed' } }));
        writeFileSync(join(directory, 'readable.md'), '# transcript\n');
        const snapshot = await api('/api/sessions/snapshot-session/snapshot');
        assert.equal(snapshot.status, 200);
        assert.deepEqual(snapshot.body.state, { loop: { status: 'completed' } });
        assert.equal(snapshot.body.readable, '# transcript\n');

        const empty = await api('/api/sessions/welcome-session/snapshot');
        assert.equal(empty.status, 200);
        assert.equal(empty.body.state, null);
    });

    it('handles heartbeat, unknown sessions, and unknown message types', async () => {
        const socket = await panel();
        socket.send({ v: 1, type: 'ping' });
        await socket.waitFor((message) => message.type === 'pong');
        socket.send({ v: 1, type: 'input', session: 'never-created', content: [] });
        const unknown = await socket.waitFor(
            (message) => message.type === 'error' && message.error === 'unknown_session');
        assert.match(unknown.message, /never-created/);
        socket.send({ v: 1, type: 'from_the_future', session: 'x' });
        socket.send({ v: 2, type: 'ping' });
        const version = await socket.waitFor(
            (message) => message.type === 'error' && message.error === 'unsupported_version');
        assert.equal(version.v, 1);
        socket.send({ v: 1, type: 'ping' });
        await socket.waitFor((message) => message.type === 'pong');
    });

    it('delivers a confirmation to a client watching a different session', async () => {
        // The defect this pins: prompts were broadcast only to the subscribers
        // of their own session. An operator looking at another session saw a
        // count in the list and nothing else, could not answer, and the worker's
        // deadline denied the call. An approval must not be missable.
        const watched = ctx.hub.registry.create('a1-watched');
        const subject = ctx.hub.registry.create('a1-subject');
        await identify(subject, 'a1-worker');

        const socket = await panel();
        socket.send({ v: 1, type: 'subscribe', session: watched.id });
        await socket.waitFor((message) => message.type === 'subscribed'
            && message.session.session_id === watched.id);

        const confirmation = await connectWorker(
            `${ctx.wsBase}/agent/${subject.id}/confirm?token=${subject.token}`);
        sockets.push(confirmation);
        confirmation.send({
            type: 'confirmation_request',
            data: {
                worker_id: 'a1-worker',
                session_id: subject.id,
                run_id: 'run-a1',
                confirmation_id: 'c-a1',
                call: { name: 'run_command', arguments: { command: 'echo hi' } },
            },
        });

        const prompt = await socket.waitFor(
            (message) => message.type === 'confirmation' && message.open === true,
            { label: 'a confirmation for a session this client never subscribed to' });
        assert.equal(prompt.session, subject.id);
        assert.equal(prompt.confirmation.confirmation_id, 'c-a1');

        // Seeing it is only half of it; answering from here is the behaviour the
        // defect removed.
        socket.send({
            v: 1, type: 'confirmation', session: subject.id,
            confirmation_id: 'c-a1', decision: 'approved', reason: 'reviewed',
        });
        const response = await confirmation.waitFor(
            (message) => message.type === 'confirmation_response');
        assert.equal(response.data.decision, 'approved');
        await confirmation.close();
    });

    it('reports one transcript epoch for the whole hub process', async () => {
        const meta = await api('/api/meta');
        const epoch = meta.body.transcript_epoch;
        assert.equal(typeof epoch, 'string', 'meta carries no transcript_epoch');
        assert.ok(epoch.length > 0);

        // Announced as a capability, so a client can check before relying on it.
        assert.ok(meta.body.capabilities.includes('transcript-epoch'));

        const socket = await panel();
        const welcome = socket.messages.find((message) => message.type === 'welcome');
        assert.equal(welcome.hub.transcript_epoch, epoch, 'welcome disagrees with meta');

        const session = ctx.hub.registry.create('epoch-session');
        socket.send({ v: 1, type: 'subscribe', session: session.id });
        const subscribed = await socket.waitFor((message) => message.type === 'subscribed'
            && message.session.session_id === session.id);
        assert.equal(subscribed.transcript_epoch, epoch,
            'subscribed disagrees with meta, so a client cannot detect a stale cursor');

        // It identifies the process, not the connection.
        const second = await panel();
        assert.equal(second.messages.find((m) => m.type === 'welcome').hub.transcript_epoch, epoch);
    });

    it('gives a second hub process a different epoch', async () => {
        // The reason the epoch exists: `hub_sequence` restarts at 1 with each
        // process, so a cursor from the first hub must not look valid to the
        // second one.
        const first = await api('/api/meta');
        const other = await startTestHub();
        try {
            const response = await fetch(`${other.base}/api/meta`);
            const body = await response.json();
            assert.notEqual(body.transcript_epoch, first.body.transcript_epoch,
                'two hub processes report the same epoch');
        } finally {
            await other.hub.stop();
        }
    });

    it('advertises its capabilities and lists them per response', async () => {
        const first = await api('/api/meta');
        assert.ok(first.body.capabilities.length > 0);

        // Each response gets its own array: handing the same one to every caller
        // lets one of them mutate it for everybody.
        first.body.capabilities.push('invented');
        const second = await api('/api/meta');
        assert.ok(!second.body.capabilities.includes('invented'),
            'a capability list was shared between responses');
    });
});

describe('panel trust boundary', () => {
    it('requires the configured token for REST and the panel socket', async () => {
        const ctx = await startTestHub({ panel: { token: 'sekret' } });
        try {
            // Metadata stays readable without a token: the panel needs it to
            // render a token prompt, and it exposes no paths or credentials.
            const meta = await fetch(`${ctx.base}/api/meta`);
            assert.equal(meta.status, 200);
            assert.equal((await meta.json()).name, 'simplex-hub');

            const denied = await fetch(`${ctx.base}/api/sessions`);
            assert.equal(denied.status, 401);
            const allowed = await fetch(`${ctx.base}/api/sessions?token=sekret`);
            assert.equal(allowed.status, 200);
            const header = await fetch(`${ctx.base}/api/sessions`, {
                headers: { Authorization: 'Bearer sekret' },
            });
            assert.equal(header.status, 200);
            assert.equal(await upgradeStatus(`${ctx.wsBase}/panel/ws`), 401);
            assert.equal(await upgradeStatus(`${ctx.wsBase}/panel/ws?token=sekret`), 101);
            // The worker-facing routes keep using session tokens.
            const session = ctx.hub.registry.create('token-boundary');
            assert.equal(await upgradeStatus(
                `${ctx.wsBase}/agent/token-boundary/events?token=${session.token}`), 101);
            assert.equal(await upgradeStatus(
                `${ctx.wsBase}/agent/token-boundary/events?token=sekret`), 401);
        } finally {
            await ctx.hub.stop();
        }
    });

    it('refuses a panel upgrade from a foreign origin', async () => {
        const ctx = await startTestHub();
        try {
            const socket = new (await import('ws')).WebSocket(`${ctx.wsBase}/panel/ws`, {
                headers: { Origin: 'http://evil.example' },
            });
            const status = await new Promise((resolve) => {
                socket.once('unexpected-response', (_request, response) => {
                    response.resume();
                    resolve(response.statusCode);
                });
                socket.once('error', () => resolve('error'));
                socket.once('open', () => resolve(101));
            });
            assert.equal(status, 403);
            socket.terminate();
        } finally {
            await ctx.hub.stop();
        }
    });
});
