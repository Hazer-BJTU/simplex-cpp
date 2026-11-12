import assert from 'node:assert/strict';
import { it, afterEach } from 'node:test';
import {
    QuestionConflictError, QuestionStore, validOptions, validQuestion,
} from '../src/state/questions.ts';
import { startTestHub } from './helpers/hub.js';
import { createPanelStore } from '../web/src/state/store.ts';
import { connectWorker, workerEvent, until } from './helpers/worker.js';

const cleanup = [];

/** Wait for a message that arrived after `from`, so a stale frame cannot match. */
async function waitAfter(peer, from, predicate, timeout = 3000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        const found = peer.messages.slice(from).find(predicate);
        if (found !== undefined) return found;
        await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error(`timed out waiting for a new message; saw ${JSON.stringify(peer.messages.slice(from))}`);
}
afterEach(async () => {
    for (const close of cleanup.splice(0).reverse()) await close();
});

it('validates questions and options before anything is published', () => {
    assert.equal(validQuestion('Which branch?'), true);
    assert.equal(validQuestion('   '), false);
    assert.equal(validQuestion('q'.repeat(8 * 1024 + 1)), false);
    assert.equal(validOptions(['A', 'B']), true);
    assert.equal(validOptions(['A']), false);
    assert.equal(validOptions(['A', 'A']), false);
    assert.equal(validOptions(['A', '']), false);
    assert.equal(validOptions(['A', 'x'.repeat(201)]), false);
});

it('settles one answer, refuses the next, and bounds pending work', async () => {
    const questions = new QuestionStore(1, 2);
    const pending = questions.open({
        session: 'demo', questionId: 'q-1', question: 'Which?', options: ['A', 'B'], timeoutMs: 5000,
    });
    assert.equal(questions.count('demo'), 1);
    assert.throws(() => questions.open({
        session: 'demo', questionId: 'q-2', question: 'Other?', options: [], timeoutMs: 5000,
    }), /too many pending questions for this session/);
    assert.deepEqual(questions.answer('other', 'q-1', { text: 'x' }), {
        ok: false, code: 'unknown_question', message: 'that question is no longer open',
    });
    assert.equal(questions.answer('demo', 'q-1', { text: 'A', option_index: 0 }).ok, false);
    assert.equal(questions.answer('demo', 'q-1', { option_index: 5 }).ok, false);
    assert.equal(questions.answer('demo', 'q-1', { option_index: 1 }).ok, true);
    assert.deepEqual(await pending, {
        outcome: 'answered', reply: { option_index: 1, option: 'B' },
    });
    assert.equal(questions.count('demo'), 0);
    assert.equal(questions.answer('demo', 'q-1', { text: 'late' }).code, 'unknown_question');

    const duplicate = new QuestionStore();
    const first = duplicate.open({
        session: 'demo', questionId: 'q-dup', question: 'First?', options: [], timeoutMs: 5000,
    });
    assert.throws(() => duplicate.open({
        session: 'demo', questionId: 'q-dup', question: 'Second?', options: [], timeoutMs: 5000,
    }), QuestionConflictError);
    duplicate.cancelAll('test');
    assert.deepEqual(await first, { outcome: 'cancelled', reason: 'test' });
});

it('tracks pending questions and refusals in the panel store', () => {
    const store = createPanelStore();
    const question = {
        question_id: 'q-store', question: 'Which?', options: [], asked_at: 1, deadline_at: 2,
    };
    store.getState().setQuestions('demo', [question]);
    assert.equal(store.getState().questions.get('demo').length, 1);
    store.getState().noteQuestionFailure('q-store', 'answer is too long');
    const first = store.getState().questionFailures.get('q-store');
    assert.equal(first.message, 'answer is too long');
    // A repeated refusal must still be a new object, so the card re-renders.
    store.getState().noteQuestionFailure('q-store', 'answer is too long');
    assert.notEqual(store.getState().questionFailures.get('q-store'), first);
    store.getState().clearQuestionFailure('q-store');
    assert.equal(store.getState().questionFailures.has('q-store'), false);
    store.getState().setQuestions('demo', []);
    assert.equal(store.getState().questions.has('demo'), false);
});

it('times out without an operator and cancels on demand', async () => {
    const questions = new QuestionStore();
    const timeout = await questions.open({
        session: 'demo', questionId: 'q-t', question: 'Anyone?', options: [], timeoutMs: 20,
    });
    assert.deepEqual(timeout, { outcome: 'timeout' });
    const cancelled = questions.open({
        session: 'demo', questionId: 'q-c', question: 'Still there?', options: [], timeoutMs: 5000,
    });
    questions.cancel('demo', 'run_cancelled');
    assert.deepEqual(await cancelled, { outcome: 'cancelled', reason: 'run_cancelled' });
});

async function setup(overrides = {}) {
    const ctx = await startTestHub({ limits: { confirmIdentityHoldMs: 120 }, ...overrides });
    cleanup.push(() => ctx.hub.http.server.listening ? ctx.hub.stop() : undefined);
    const session = ctx.hub.registry.create('demo', {});
    const endpoints = ctx.hub.supervisor.endpointsFor(session.id, session.token);
    const event = await connectWorker(endpoints.events);
    cleanup.push(() => event.close());
    let sequence = 0;
    const emit = (name, run = 'run-1', data = {}) => event.send(workerEvent({
        session: 'demo', worker: 'worker-1', sequence: ++sequence, event: name, runId: run, data,
    }));
    const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
    cleanup.push(() => panel.close());
    await panel.waitFor((message) => message.type === 'welcome');
    panel.send({ type: 'subscribe', session: 'demo' });
    await panel.waitFor((message) => message.type === 'subscribed');
    // One admitted run is enough; every call below reuses run-1.
    emit('input_admitted');
    const call = async (arguments_, requestId) => {
        const url = new URL(endpoints.tools);
        url.pathname += '/ask_user/ask';
        const peer = await connectWorker(url.href);
        peer.send({ type: 'tool_request', data: {
            worker_id: 'worker-1', session_id: 'demo', run_id: 'run-1',
            request_id: requestId, arguments: arguments_,
        } });
        const response = await peer.waitFor((value) => value.type === 'tool_response');
        await peer.waitForClose();
        return response.data;
    };
    const subscribe = async () => {
        const peer = await connectWorker(`${ctx.wsBase}/panel/ws`);
        cleanup.push(() => peer.close());
        await peer.waitFor((message) => message.type === 'welcome');
        peer.send({ type: 'subscribe', session: 'demo' });
        await peer.waitFor((message) => message.type === 'subscribed');
        return peer;
    };
    return { ...ctx, session, endpoints, event, emit, panel, call, subscribe };
}

it('publishes the question, accepts an option, and clears the panel set', async () => {
    const ctx = await setup();
    const pending = ctx.call({ question: 'Which branch?', options: ['main', 'next'], timeout_ms: 5000 },
        'question-1');
    const opened = await ctx.panel.waitFor((message) => message.type === 'questions'
        && message.questions.length === 1);
    assert.equal(opened.questions[0].question_id, 'question-1');
    assert.equal(opened.questions[0].question, 'Which branch?');
    assert.deepEqual(opened.questions[0].options, ['main', 'next']);
    assert.ok(opened.questions[0].deadline_at > opened.questions[0].asked_at);

    ctx.panel.send({ type: 'question_answer', session: 'demo', question_id: 'question-1',
        answer: { option_index: 1 } });
    await ctx.panel.waitFor((message) => message.type === 'accepted'
        && message.action === 'question_answer');
    const response = await pending;
    assert.equal(response.status, 'succeeded');
    assert.equal(response.result.outcome, 'answered');
    assert.deepEqual(response.result.reply, { option_index: 1, option: 'next' });
    await ctx.panel.waitFor((message) => message.type === 'questions'
        && message.questions.length === 0);
});

it('accepts free text even when options were offered, and rejects stale answers', async () => {
    const ctx = await setup();
    const pending = ctx.call({ question: 'Pick one', options: ['A', 'B'], timeout_ms: 5000 },
        'question-2');
    await ctx.panel.waitFor((message) => message.type === 'questions'
        && message.questions.length === 1);
    ctx.panel.send({ type: 'question_answer', session: 'demo', question_id: 'question-2',
        answer: { text: 'neither, use C' } });
    const response = await pending;
    assert.deepEqual(response.result.reply, { text: 'neither, use C' });
    ctx.panel.send({ type: 'question_answer', session: 'demo', question_id: 'question-2',
        answer: { text: 'again' } });
    const refused = await ctx.panel.waitFor((message) => message.type === 'error'
        && message.error === 'unknown_question');
    assert.equal(refused.question_id, 'question-2');
});

it('times out as a normal result and withdraws on a cancelled run', async () => {
    const ctx = await setup();
    const pending = ctx.call({ question: 'Anyone?', timeout_ms: 1000 }, 'question-3');
    const response = await pending;
    assert.equal(response.status, 'succeeded');
    assert.equal(response.result.outcome, 'timeout');
    assert.ok(response.result.elapsed_ms >= 1000);
    await ctx.panel.waitFor((message) => message.type === 'questions'
        && message.questions.length === 0);

    const mark = ctx.panel.messages.length;
    const waiting = ctx.call({ question: 'Still needed?', timeout_ms: 5000 }, 'question-4');
    await waitAfter(ctx.panel, mark, (message) => message.type === 'questions'
        && message.questions.length === 1);
    ctx.panel.send({ type: 'signal', session: 'demo', operation: 'cancel', run_id: 'run-1' });
    await ctx.panel.waitFor((message) => message.type === 'accepted' && message.action === 'signal');
    const cancelled = await waiting;
    assert.equal(cancelled.result.outcome, 'cancelled');
    assert.equal(cancelled.result.reason, 'run_cancelled');
});

it('clamps elapsed time when the wall clock steps backwards', async (t) => {
    t.mock.timers.enable({ apis: ['Date'], now: 1_000_000 });
    t.after(() => t.mock.timers.reset());
    const ctx = await setup();
    const pending = ctx.call({ question: 'What time is it?', timeout_ms: 5000 }, 'question-clock');
    await ctx.panel.waitFor((message) => message.type === 'questions'
        && message.questions.length === 1);
    // A clock correction during the wait must not produce a negative duration.
    t.mock.timers.setTime(900_000);
    ctx.panel.send({ type: 'question_answer', session: 'demo', question_id: 'question-clock',
        answer: { text: 'late' } });
    const response = await pending;
    assert.equal(response.result.outcome, 'answered');
    assert.equal(response.result.elapsed_ms, 0);
});

it('refuses headless callers instead of waiting for an operator', async () => {
    const ctx = await setup();
    ctx.session.kind = 'headless';
    const mark = ctx.panel.messages.length;
    const response = await ctx.call({ question: 'Anyone?', timeout_ms: 5000 }, 'question-headless');
    assert.equal(response.status, 'rejected');
    assert.equal(response.error.code, 'not_implemented');
    await new Promise((resolve) => setTimeout(resolve, 50));
    assert.equal(ctx.panel.messages.slice(mark).some((message) => message.type === 'questions'), false);
});

it('clamps a longer request and keeps the socket open for the granted wait', async () => {
    const ctx = await setup({ toolRequests: {
        port: 0, timeoutMs: 1000, questionTimeoutMs: 1500,
    } });
    const started = Date.now();
    const response = await ctx.call({ question: 'Anyone?', timeout_ms: 60000 }, 'question-clamp');
    assert.equal(response.result.outcome, 'timeout');
    assert.equal(response.result.timeout_ms, 1500);
    assert.ok(Date.now() - started >= 1500);
});

it('includes pending counts in subscriptions and session lists', async () => {
    const ctx = await setup();
    const pending = ctx.call({ question: 'Which?', timeout_ms: 5000 }, 'question-count');
    await ctx.panel.waitFor((message) => message.type === 'questions'
        && message.questions.length === 1);
    const peer = await ctx.subscribe();
    const snapshot = await peer.waitFor((message) => message.type === 'subscribed'
        && message.session.pending_questions === 1);
    assert.equal(snapshot.questions.length, 1);
    peer.send({ type: 'list_sessions' });
    const listed = await peer.waitFor((message) => message.type === 'sessions');
    assert.equal(listed.sessions.find((session) => session.session_id === 'demo')
        .pending_questions, 1);
    peer.send({ type: 'status_snapshot', session: 'demo' });
    const state = await peer.waitFor((message) => message.type === 'snapshot');
    assert.equal(state.session.pending_questions, 1);
    ctx.panel.send({ type: 'question_answer', session: 'demo', question_id: 'question-count',
        answer: { text: 'this one' } });
    await pending;
});

it('rejects an explicit empty options array', async () => {
    const ctx = await setup();
    const response = await ctx.call({ question: 'Which?', options: [], timeout_ms: 5000 },
        'question-empty-options');
    assert.equal(response.error.code, 'invalid_arguments');
});

it('rejects oversized or malformed requests without publishing them', async () => {
    const ctx = await setup();
    const pending = ctx.call({ question: '  ', timeout_ms: 5000 }, 'question-5');
    const response = await pending;
    assert.equal(response.status, 'rejected');
    assert.equal(response.error.code, 'invalid_arguments');
    const tooShort = await ctx.call({ question: 'Why?', timeout_ms: 0 }, 'question-6');
    assert.equal(tooShort.error.code, 'invalid_arguments');
});
