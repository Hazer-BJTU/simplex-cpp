/** Query budgets and canonical reconciliation under a continuously producing worker. */
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { it } from 'node:test';
import { Session } from '../src/state/registry.ts';
import { ConversationProjection } from '../src/subagents/conversation.ts';

const part = raw => ({ type: 'text', modality: 'text', raw });
const turn = (answer = 'answer') => ({ index: 0, user: [part('task')],
    steps: [{ index: 0, content: [part(answer)] }] });

/** Use registry event bookkeeping and virtual deadlines, without timing assertions. */
function fixture(t, active = false) {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const directory = mkdtempSync(join(tmpdir(), 'simplex-refresh-scheduling-'));
    const session = new Session({ id: 'refresh-scheduling' });
    session.kind = 'headless';
    session.noteIdentity('worker');
    session.workerCapabilities = { workerId: 'worker', names: ['session-history'] };
    session.activeRunId = active ? 'run' : '';
    const queries = [];
    const connection = { isOpen: true,
        sendPayload(payload) { queries.push(payload.data); return { ok: true }; } };
    session.connection = connection;
    const view = new ConversationProjection(session, join(directory, 'conversation.json'), 128 * 1024);
    let sequence = 0;
    function event(name, data = {}, overrides = {}) {
        const envelope = { event: name, worker_id: 'worker', request_id: 'task',
            run_id: 'run', sequence: ++sequence, data, ...overrides };
        sequence = envelope.sequence;
        session.noteEnvelope(envelope);
        view.event(envelope, session.connection);
    }
    function status(active) {
        event('status', { active, capabilities: ['session-history'] });
    }
    function reply(fields = {}, query = queries.at(-1), overrides = {}) {
        event('history', { request_id: query.request_id, revision: 1,
            start: query.start, step: query.step, total: 1, next: 1, next_step: 0,
            turns: [turn()], ...fields }, overrides);
    }
    view.connectionChanged(connection);
    t.after(() => {
        view.stop();
        t.mock.timers.reset();
        rmSync(directory, { recursive: true, force: true });
    });
    return { view, session, connection, queries, event, status, reply,
        tick: milliseconds => t.mock.timers.tick(milliseconds) };
}

it('finishes validating an in-flight snapshot without regressing live dialogue, then catches up once idle', t => {
    const { view, session, queries, event, status, reply, tick } = fixture(t);
    tick(25);
    assert.equal(queries.length, 1);
    session.trackRequest('task', 'message');
    view.trackInput('task', [part('task')]);
    event('input_admitted');
    event('input_committed');
    reply({ next: 0, next_step: 1, turns: [turn('old fragment')] });
    assert.equal(queries.length, 2, 'a live commit does not restart validated pagination');
    event('model_response', { content: [part('live answer 0')] });
    reply({ turns: [{ index: 0, user: [part('task')],
        steps: [{ index: 1, content: [part('old tail')] }] }] });
    assert.equal(view.refresh, null, 'the valid older snapshot finishes assembly');
    assert.equal(view.value.turns[0].steps[0].content[0].raw, 'live answer 0');
    assert.equal(view.value.stale, true, 'an older snapshot cannot claim the live projection is current');

    for (let index = 1; index < 500; index += 1) {
        event('model_response', { content: [part(`live answer ${index}`)] });
        status(true);
        tick(10);
    }
    assert.equal(queries.length, 2, '500 live commits and status polling cannot create active-run queries');
    assert.equal(view.value.turns[0].steps.at(-1).content[0].raw, 'live answer 499');
    assert.equal(view.value.turns[0].steps.length, 32);
    event('run_finished');
    tick(25);
    assert.equal(queries.length, 3);
    reply({ revision: 9, next: 0, next_step: 1,
        turns: [{ ...turn('live answer 0'), omitted_steps: 499 }] });
    assert.equal(queries.at(-1).step, 468, 'settlement retains the existing newest-step policy');
    reply({ revision: 9, turns: [{ index: 0, user: [part('task')],
        steps: Array.from({ length: 32 }, (_, index) => ({ index: index + 468,
            content: [part(`live answer ${index + 468}`)] })) }] });
    assert.equal(view.value.stale, false);
    assert.equal(view.value.revision, 9);
    assert.equal(view.value.turns[0].steps.at(-1).content[0].raw, 'live answer 499');
    for (let index = 0; index < 100; index += 1) { status(false); tick(25); }
    assert.equal(queries.length, 4, 'history reply sequences are not mistaken for missing live events');
});

it('defers startup/reconnect queries during active work and catches a missed settlement through status', t => {
    const { view, connection, queries, event, status, reply, tick } = fixture(t, true);
    event('ready', { capabilities: ['session-history'] });
    for (let index = 0; index < 100; index += 1) { status(true); tick(25); }
    assert.equal(queries.length, 0);
    view.connectionChanged(null);
    view.connectionChanged(connection);
    tick(100);
    assert.equal(queries.length, 0);
    status(false);
    tick(25);
    assert.equal(queries.length, 1);
    reply();
    assert.equal(view.value.stale, false);
    assert.equal(view.value.turns[0].steps[0].content[0].raw, 'answer');
});

it('rechecks idleness at dispatch when admission follows a queued refresh', t => {
    const { queries, event, reply, tick } = fixture(t);
    event('input_admitted');
    tick(25);
    assert.equal(queries.length, 0);
    event('run_finished');
    tick(25);
    assert.equal(queries.length, 1);
    reply();
});

for (const failure of ['history_error', 'timeout', 'send']) {
    it(`bounds ${failure} retries despite continuous idle status polling, and renews the budget at settlement`, t => {
        const { view, session, connection, queries, event, status, reply, tick } = fixture(t);
        if (failure === 'send') {
            connection.sendPayload = payload => { queries.push(payload.data); return { ok: false, error: 'closed' }; };
        }
        for (let attempt = 0; attempt < 3; attempt += 1) {
            tick(25);
            assert.equal(queries.length, attempt + 1);
            if (failure === 'history_error') {
                event('history_error', { request_id: queries.at(-1).request_id });
            } else if (failure === 'timeout') tick(3000);
            for (let index = 0; index < 100; index += 1) status(false);
        }
        for (let index = 0; index < 100; index += 1) { status(false); tick(100); }
        assert.equal(queries.length, 3, 'polling cannot reset a failed refresh budget');
        assert.equal(view.value.stale, true);
        assert.equal(view.value.incomplete, true);
        connection.sendPayload = payload => { queries.push(payload.data); return { ok: true }; };
        session.trackRequest('task', 'continue');
        event('input_admitted');
        status(true);
        tick(100);
        assert.equal(queries.length, 3);
        // Recover even if run_finished was missed while this channel remained open.
        status(false);
        tick(25);
        assert.equal(queries.length, 4);
        reply({ revision: 2 });
        assert.equal(view.value.stale, false);
    });
}

it('rejects a changed page revision during a run and only retries after settlement', t => {
    const { view, queries, event, reply, tick } = fixture(t);
    tick(25);
    view.value.turns = [turn('known result')];
    reply({ next: 0, next_step: 1 });
    event('input_admitted');
    reply({ revision: 2, turns: [{ index: 0, user: [part('task')],
        steps: [{ index: 1, content: [part('invalid revision')] }] }] });
    assert.equal(view.value.turns[0].steps[0].content[0].raw, 'known result');
    tick(4000);
    assert.equal(queries.length, 2);
    event('run_finished');
    tick(25);
    reply({ revision: 3 });
    assert.equal(view.value.stale, false);
});

it('preserves known dialogue after an event gap and reconciles without mixing its older snapshot', t => {
    const { view, queries, event, reply, tick } = fixture(t);
    tick(25);
    event('tool_calls');
    reply({ next: 0, next_step: 1 });
    view.value.turns = [turn('known result')];
    event('tool_results', {}, { sequence: 4 }); // Missing sequence 3 might have committed new content.
    reply({ turns: [{ index: 0, user: [part('task')],
        steps: [{ index: 1, content: [part('old snapshot')] }] }] });
    assert.equal(view.value.turns[0].steps[0].content[0].raw, 'known result');
    assert.equal(view.value.incomplete, true);
    tick(25);
    assert.equal(queries.length, 3);
    reply({ revision: 2, turns: [turn('canonical answer')] });
    assert.equal(view.value.stale, false);
    assert.equal(view.value.turns[0].steps[0].content[0].raw, 'canonical answer');
});

it('rejects a snapshot from a superseded worker incarnation even on the same connection', t => {
    const { view, session, queries, reply, tick } = fixture(t);
    tick(25);
    session.noteIdentity('replacement-worker');
    session.workerCapabilities = { workerId: 'replacement-worker', names: ['session-history'] };
    reply();
    assert.equal(view.value.turns.length, 0);
    tick(25);
    assert.equal(queries.length, 2);
    reply({ revision: 0, turns: [turn('replacement answer')] }, queries.at(-1),
        { worker_id: 'replacement-worker' });
    assert.equal(view.value.worker_id, 'replacement-worker');
    assert.equal(view.value.turns[0].steps[0].content[0].raw, 'replacement answer');
});

it('supersedes pre-compact pagination without accepting a delayed correlated reply', t => {
    const { view, queries, event, reply, tick } = fixture(t);
    tick(25);
    reply({ next: 0, next_step: 1 });
    const obsolete = queries.at(-1);
    view.value.turns = [turn('known result')];
    event('input_admitted');
    event('compact_finished', { durable: true, revision: 2 });
    reply({ turns: [{ index: 0, user: [part('task')],
        steps: [{ index: 1, content: [part('obsolete result')] }] }] }, obsolete);
    assert.equal(view.refresh, null);
    assert.equal(view.value.turns[0].steps[0].content[0].raw, 'known result');
    tick(4000);
    assert.equal(queries.length, 2);
    event('run_finished');
    tick(25);
    reply({ revision: 2, total: 0, next: 0, turns: [] });
    assert.equal(view.value.stale, false);
    assert.deepEqual(view.value.turns, []);
});

it('cancels queued and in-flight refreshes at disconnect and stop boundaries', t => {
    const { view, session, connection, queries, status, reply, tick } = fixture(t);
    view.connectionChanged(null);
    tick(25);
    assert.equal(queries.length, 0);
    view.connectionChanged(connection);
    tick(25);
    const obsolete = queries.at(-1);
    view.connectionChanged(null);
    tick(4000);
    assert.equal(queries.length, 1);
    const replacement = { ...connection };
    session.connection = replacement;
    view.connectionChanged(replacement);
    tick(25);
    reply({}, obsolete);
    assert.equal(view.value.turns.length, 0, 'old correlation cannot publish into the replacement channel');
    reply();
    assert.equal(view.value.stale, false);
    status(true);
    status(false);
    view.stop();
    tick(10000);
    assert.equal(queries.length, 2, 'shutdown cancels the queued settlement refresh');
});
