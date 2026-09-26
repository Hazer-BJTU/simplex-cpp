/**
 * @file tool-confirmation exchanges: identity judgement, the bounded hold for
 * an unidentified worker, single-response discipline, and prompt retirement.
 */
import assert from 'node:assert/strict';
import { after, before, describe, it } from 'node:test';
import { PROMPT_STATE, awaitWorkerIdentity } from '../src/worker/confirmation.ts';
import { IDENTITY } from '../src/state/registry.ts';
import { connectWorker, upgradeStatus, until, workerEvent } from './helpers/worker.js';
import { startTestHub } from './helpers/hub.js';

/** A confirmation request with overridable identifiers. */
function request({ session, worker = 'worker-1', run = 'run-1', id = 'c-1', call, extra = {} }) {
    return {
        type: 'confirmation_request',
        data: {
            worker_id: worker,
            session_id: session,
            run_id: run,
            confirmation_id: id,
            call: call ?? {
                type: 'serial_write',
                security: 'require_confirm',
                id: 'call-001',
                name: 'run_command',
                arguments: { command: "printf 'hello\\n'" },
            },
            ...extra,
        },
    };
}

describe('tool confirmation exchanges', () => {
    let ctx;
    const opened = [];
    const prompts = [];
    const settled = [];
    let counter = 0;

    before(async () => {
        ctx = await startTestHub({}, {
            onPrompt: (prompt) => prompts.push(prompt),
            onPromptSettled: (prompt, outcome) => settled.push({ prompt, outcome }),
        });
    });

    after(async () => {
        for (const socket of opened) await socket.close();
        await ctx.hub.stop();
    });

    function session(label) {
        counter += 1;
        return ctx.hub.registry.create(`${label}-${counter}`);
    }

    /** Connect an event connection and make it identify as `workerId`. */
    async function identify(target, workerId = 'worker-1') {
        const worker = await connectWorker(
            `${ctx.wsBase}/agent/${target.id}/events?token=${target.token}`);
        opened.push(worker);
        worker.send(workerEvent({
            session: target.id,
            worker: workerId,
            sequence: 1,
            event: 'status',
            data: { active: false },
        }));
        await until(() => target.identity.state === IDENTITY.live, { label: 'live identity' });
        return worker;
    }

    /** Open a confirmation connection and send one request. */
    async function confirm(target, overrides = {}) {
        const socket = await connectWorker(
            `${ctx.wsBase}/agent/${target.id}/confirm?token=${target.token}`);
        opened.push(socket);
        socket.send(request({ session: target.id, ...overrides }));
        return socket;
    }

    /** Wait for the session's only open prompt. */
    async function openPrompt(target) {
        await until(() => target.prompts.size === 1, { label: 'an open prompt' });
        return [...target.prompts.values()][0];
    }

    it('answers a verified request with all four identifiers', async () => {
        const target = session('verified');
        await identify(target, 'worker-1');
        const socket = await confirm(target);
        const prompt = await openPrompt(target);
        assert.equal(prompt.state, PROMPT_STATE.decision);
        assert.equal(prompt.verified, true);
        assert.equal(prompt.call.name, 'run_command');
        assert.equal(prompts.at(-1), prompt);

        assert.deepEqual(prompt.decide('approved', 'looks safe'), { ok: true });
        const response = await socket.waitFor((message) => message.type === 'confirmation_response');
        assert.deepEqual(response.data, {
            worker_id: 'worker-1',
            session_id: target.id,
            run_id: 'run-1',
            confirmation_id: 'c-1',
            decision: 'approved',
            reason: 'looks safe',
        });
        await socket.close();
        await until(() => target.prompts.size === 0, { label: 'prompt cleanup' });
        assert.equal(settled.at(-1).outcome.phase, 'decided');
    });

    it('carries a denial and its reason', async () => {
        const target = session('denied');
        await identify(target, 'worker-1');
        const socket = await confirm(target, { id: 'c-deny' });
        const prompt = await openPrompt(target);
        assert.deepEqual(prompt.decide('denied', 'not now'), { ok: true });
        const response = await socket.waitFor((message) => message.type === 'confirmation_response');
        assert.equal(response.data.decision, 'denied');
        assert.equal(response.data.reason, 'not now');
        await socket.close();
    });

    it('answers a prompt only once', async () => {
        const target = session('once');
        await identify(target, 'worker-1');
        const socket = await confirm(target, { id: 'c-once' });
        const prompt = await openPrompt(target);
        assert.deepEqual(prompt.decide('approved'), { ok: true });
        const second = prompt.decide('denied');
        assert.equal(second.ok, false);
        assert.match(second.error, /already answered/);
        const response = await socket.waitFor((message) => message.type === 'confirmation_response');
        assert.equal(response.data.decision, 'approved');
        await socket.close();
    });

    it('denies a request whose worker does not own the session', async () => {
        const target = session('mismatch');
        await identify(target, 'worker-1');
        const socket = await confirm(target, { worker: 'worker-elsewhere', id: 'c-mismatch' });
        const response = await socket.waitFor((message) => message.type === 'confirmation_response');
        assert.equal(response.data.decision, 'denied');
        assert.match(response.data.reason, /identity mismatch/);
        assert.equal(target.prompts.size, 0);
        await socket.close();
    });

    it('holds an unidentified request until the event connection identifies itself', async () => {
        const target = session('hold');
        const socket = await confirm(target, { id: 'c-hold' });
        // Nothing has identified the worker yet, so nothing may be decided.
        await new Promise((resolve) => setTimeout(resolve, 50));
        assert.equal(target.prompts.size, 0);
        assert.equal(socket.messages.length, 0);

        const worker = await identify(target, 'worker-late');
        worker.send(workerEvent({
            session: target.id,
            worker: 'worker-late',
            sequence: 2,
            event: 'status',
        }));
        // The request claims worker-1 while the live connection is worker-late.
        const response = await socket.waitFor((message) => message.type === 'confirmation_response');
        assert.equal(response.data.decision, 'denied');
        assert.match(response.data.reason, /worker-late/);
        await socket.close();
    });

    it('verifies a held request when the claimed worker arrives', async () => {
        const target = session('hold-ok');
        const socket = await confirm(target, { worker: 'worker-slow', id: 'c-hold-ok' });
        await new Promise((resolve) => setTimeout(resolve, 30));
        assert.equal(target.prompts.size, 0);
        await identify(target, 'worker-slow');
        const prompt = await openPrompt(target);
        assert.equal(prompt.verified, true);
        assert.deepEqual(prompt.decide('approved'), { ok: true });
        const response = await socket.waitFor((message) => message.type === 'confirmation_response');
        assert.equal(response.data.decision, 'approved');
        await socket.close();
    });

    it('denies when the hold window expires without an identity', async () => {
        const short = await startTestHub({ limits: { confirmIdentityHoldMs: 150 } }, {});
        try {
            const target = short.hub.registry.create('hold-expired');
            const socket = await connectWorker(
                `${short.wsBase}/agent/${target.id}/confirm?token=${target.token}`);
            socket.send(request({ session: target.id, id: 'c-expired' }));
            const response = await socket.waitFor(
                (message) => message.type === 'confirmation_response', { timeout: 2000 });
            assert.equal(response.data.decision, 'denied');
            assert.match(response.data.reason, /not verified/);
            await socket.close();
        } finally {
            await short.hub.stop();
        }
    });

    it('retires a prompt when the worker closes the connection', async () => {
        const target = session('retire');
        await identify(target, 'worker-1', 'worker-1');
        const socket = await confirm(target, { id: 'c-retire' });
        await openPrompt(target);
        const before = settled.length;
        await socket.close();
        await until(() => settled.length > before, { label: 'a settled prompt' });
        assert.equal(settled.at(-1).outcome.phase, 'disconnected');
        assert.equal(settled.at(-1).prompt.state, PROMPT_STATE.retired);
        await until(() => target.prompts.size === 0);
    });

    it('rejects a second application frame', async () => {
        const target = session('two-frames');
        await identify(target, 'worker-1');
        const socket = await confirm(target, { id: 'c-two' });
        await openPrompt(target);
        socket.send(request({ session: target.id, id: 'c-two-b' }));
        const closed = await socket.waitForClose();
        assert.equal(closed.code, 1008);
        assert.ok(target.stats.protocolErrors >= 1);
        await until(() => target.prompts.size === 0, { label: 'prompt cleanup' });
    });

    it('refuses a duplicate confirmation id without disturbing the first', async () => {
        const target = session('duplicate');
        await identify(target, 'worker-1');
        const first = await confirm(target, { id: 'c-dup' });
        const prompt = await openPrompt(target);
        const second = await confirm(target, { id: 'c-dup' });
        const closed = await second.waitForClose();
        assert.equal(closed.code, 1008);
        assert.equal(target.prompts.get('c-dup'), prompt);
        assert.deepEqual(prompt.decide('denied'), { ok: true });
        const response = await first.waitFor((message) => message.type === 'confirmation_response');
        assert.equal(response.data.decision, 'denied');
        await first.close();
    });

    it('aborts a malformed request without answering it', async () => {
        const target = session('malformed');
        await identify(target, 'worker-1');
        const cases = [
            { type: 'confirmation_response', data: {} },
            request({ session: 'another-session', id: 'c-x' }),
            request({ session: target.id, id: '' }),
            { type: 'confirmation_request', data: { worker_id: 'worker-1', session_id: target.id } },
        ];
        for (const [index, body] of cases.entries()) {
            const socket = await connectWorker(
                `${ctx.wsBase}/agent/${target.id}/confirm?token=${target.token}`);
            opened.push(socket);
            socket.send(body);
            const closed = await socket.waitForClose();
            assert.equal(closed.code, 1008, `case ${index}`);
            assert.equal(socket.messages.filter(
                (message) => message.type === 'confirmation_response').length, 0);
        }
        assert.equal(target.prompts.size, 0);
    });

    it('rejects a binary request', async () => {
        const target = session('binary-request');
        await identify(target, 'worker-1');
        const socket = await connectWorker(
            `${ctx.wsBase}/agent/${target.id}/confirm?token=${target.token}`);
        opened.push(socket);
        socket.sendBinary(Buffer.from(JSON.stringify(request({ session: target.id }))));
        const closed = await socket.waitForClose();
        assert.equal(closed.code, 1008);
        assert.equal(target.prompts.size, 0);
    });

    it('secures the confirmation route like the event route', async () => {
        const target = session('secured');
        assert.equal(
            await upgradeStatus(`${ctx.wsBase}/agent/${target.id}/confirm?token=wrong`), 401);
        assert.equal(
            await upgradeStatus(`${ctx.wsBase}/agent/nobody/confirm?token=x`), 404);
        assert.equal(
            await upgradeStatus(`${ctx.wsBase}/agent/bad%2Fid/confirm?token=x`), 404);
    });

    it('retires every open prompt when the hub stops', async () => {
        const stopping = await startTestHub({}, {});
        const target = stopping.hub.registry.create('shutdown-prompt');
        const worker = await connectWorker(
            `${stopping.wsBase}/agent/${target.id}/events?token=${target.token}`);
        worker.send(workerEvent({ session: target.id, worker: 'worker-stop', sequence: 1 }));
        await until(() => target.identity.state === IDENTITY.live);
        const socket = await connectWorker(
            `${stopping.wsBase}/agent/${target.id}/confirm?token=${target.token}`);
        socket.send(request({ session: target.id, worker: 'worker-stop', id: 'c-stop' }));
        await until(() => target.prompts.size === 1);
        await stopping.hub.stop();
        assert.equal(target.prompts.size, 0);
        await socket.close();
        await worker.close();
    });
});

describe('awaitWorkerIdentity', () => {
    /** Minimal session double exposing only what the judge reads. */
    function fakeSession(identity) {
        const listeners = new Set();
        return {
            identity,
            onIdentityChange(listener) {
                listeners.add(listener);
                return () => listeners.delete(listener);
            },
            transition(next) {
                this.identity = next;
                for (const listener of listeners) listener(next);
            },
        };
    }

    it('accepts a live matching identity immediately', async () => {
        const session = fakeSession({ state: IDENTITY.live, workerId: 'w1' });
        assert.deepEqual(await awaitWorkerIdentity(session, 'w1', 1000), { ok: true, held: false });
    });

    it('rejects a live different identity immediately', async () => {
        const session = fakeSession({ state: IDENTITY.live, workerId: 'w2' });
        const verdict = await awaitWorkerIdentity(session, 'w1', 1000);
        assert.equal(verdict.ok, false);
        assert.match(verdict.reason, /mismatch/);
    });

    it('waits through stale and unknown identities', async () => {
        for (const state of [IDENTITY.unknown, IDENTITY.stale]) {
            const session = fakeSession({ state, workerId: null, lastWorkerId: 'w-old' });
            const pending = awaitWorkerIdentity(session, 'w1', 2000);
            session.transition({ state: IDENTITY.live, workerId: 'w1' });
            assert.deepEqual(await pending, { ok: true, held: true });
        }
    });

    it('fails closed when the hold window is zero', async () => {
        const session = fakeSession({ state: IDENTITY.unknown, workerId: null });
        const verdict = await awaitWorkerIdentity(session, 'w1', 0);
        assert.equal(verdict.ok, false);
        assert.match(verdict.reason, /not verified/);
    });
});
