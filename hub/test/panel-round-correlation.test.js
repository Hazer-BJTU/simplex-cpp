/** Execution grouping must not follow whichever input arrived most recently. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { buildRounds } from '../web/src/app/rounds.ts';
import { createPanelStore } from '../web/src/state/store.ts';

const session = {
    session_id: 'demo', created_at: '2026-10-04T00:00:00.000Z', spec: {}, connected: true,
    identity: { state: 'live', worker_id: 'worker-1', since: null },
    stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 1 },
    last_run_id: '', last_event_at: null, last_event: null, confirmations: [], process: null, requests: [],
};

function addInput(store, id) {
    store.getState().beginInput('demo', id, [{ type: 'text', modality: 'text', raw: id }], 'message');
    store.getState().applyRequest({ type: 'request', session: 'demo', request: {
        request_id: id, operation: 'message', state: 'sent', detail: null,
        sent_at: '2026-10-04T00:00:00.000Z', observed_at: null,
    } });
}

function storeWithInputs(ids = ['A', 'B', 'C']) {
    const store = createPanelStore();
    store.getState().applyWelcome({ type: 'welcome', sessions: [session], subscriptions: [],
        hub: { name: 'simplex-hub', version: 'test', protocol: { name: 'simplex-hub-panel', version: 1 },
            worker_protocol: '1', capabilities: [], transcript_epoch: 'epoch-1' } });
    for (const id of ids) addInput(store, id);
    return store;
}

function envelope(sequence, name, request, data, extra = {}) {
    return { type: 'event', event: name, session_id: 'demo', worker_id: 'worker-1',
        request_id: request, run_id: `run-${request}`, sequence, hub_sequence: sequence,
        received_at: `2026-10-04T00:00:${String(sequence).padStart(2, '0')}.000Z`, data, ...extra };
}

function response(text, id) {
    return { type: 'model_response', role: 'assistant', content: [{ type: 'text', raw: text }],
        invokes: [{ id, name: 'run_command', arguments: { command: text } }] };
}

function result(id, text) {
    return { type: 'invoke_return', role: 'tool', content: [{ type: 'text', raw: text }],
        invoke_return: { query: { id, name: 'run_command', arguments: {} },
            output: { type: 'text', raw: text } } };
}

function transcript() {
    return [
        envelope(1, 'input_admitted', 'A', { operation: 'message' }),
        envelope(2, 'run_started', 'A', {}),
        envelope(3, 'input_rejected', 'C', { request_id: 'C', operation: 'message',
            code: 'payload_queue_full', message: 'Wait for current work to finish, then retry.' }, { run_id: '' }),
        envelope(4, 'model_response', 'A', response('Answer A', 'call-A')),
        envelope(5, 'tool_calls', 'A', [{ id: 'call-A', name: 'run_command', arguments: {} }]),
        envelope(6, 'tool_results', 'A', [result('call-A', 'Output A')]),
        envelope(7, 'run_finished', 'A', { status: 'completed', exchanges: 1 }),
        envelope(8, 'input_admitted', 'B', { operation: 'message' }),
        envelope(9, 'run_started', 'B', {}),
        envelope(10, 'model_response', 'B', response('Answer B', 'call-B')),
        envelope(11, 'tool_results', 'B', [result('call-B', 'Output B')]),
        envelope(12, 'run_finished', 'B', { status: 'exchange_limit', exchanges: 1 }),
    ];
}

function roundsFor(store) {
    const view = store.getState().views.get('demo');
    return buildRounds(view.items, view.confirmations, view.requests);
}

function verify(rounds, replay = false) {
    const requestOf = (round) => round.input?.requestId ?? round.admitted?.envelope.request_id
        ?? (round.problems.length ? 'C' : '');
    const byRequest = new Map(rounds.map((round) => [requestOf(round), round]));
    assert.equal(rounds.length, 3);
    for (const [id, status] of [['A', 'completed'], ['B', 'exchange_limit']]) {
        const round = byRequest.get(id);
        assert.equal(round.kind, 'run');
        assert.equal(round.status, status);
        assert.equal(round.open, false);
        assert.deepEqual(round.assistant.map((block) => block.text), [`Answer ${id}`]);
        assert.equal(round.calls.length, 1);
        assert.equal(round.calls[0].id, `call-${id}`);
        assert.equal(round.calls[0].result.text, `Output ${id}`);
        assert.equal(round.calls[0].status, 'ok');
        assert.equal(round.problems.length, 0);
        assert.equal(round.protocol.find((item) => item.envelope.event === 'run_finished')
            .envelope.request_id, id);
        if (!replay) assert.equal(round.input.parts[0].raw, id);
    }
    assert.deepEqual(rounds.filter((round) => round.kind === 'run').map((round) => round.index), [1, 2]);
    const rejected = byRequest.get('C');
    assert.equal(rejected.kind, 'prelude');
    assert.equal(rejected.status, 'rejected');
    assert.equal(rejected.open, false);
    assert.equal(rejected.assistant.length, 0);
    assert.equal(rejected.calls.length, 0);
    assert.equal(rejected.problems[0].label, 'Input queue full');
    if (!replay) assert.equal(rejected.input.state, 'rejected');
}

it('keeps A executing, B queued, and C rejected at every live delivery boundary', () => {
    const store = storeWithInputs();
    // Sending alone must not invent an active run for any of these inputs.
    assert.equal(roundsFor(store).every((round) => round.kind === 'prelude' && !round.open), true);
    for (const event of transcript()) {
        store.getState().applyEvent({ type: 'event', session: 'demo', hub_seq: event.hub_sequence,
            envelope: event });
        if (event.sequence === 6 || event.sequence === 7) {
            const rounds = roundsFor(store);
            const active = rounds.find((round) => round.input?.requestId === 'A');
            const pending = rounds.find((round) => round.input?.requestId === 'B');
            assert.equal(active.assistant[0].text, 'Answer A');
            assert.equal(active.calls[0].result.text, 'Output A');
            assert.equal(pending.kind, 'prelude');
            assert.equal(pending.open, false);
            assert.equal(pending.assistant.length, 0);
            assert.equal(pending.calls.length, 0);
            assert.equal(pending.status, '');
        }
    }
    verify(roundsFor(store));
});

it('preserves all three inputs across paged reconnect replay, including queued B', () => {
    const store = storeWithInputs();
    for (const page of [transcript().slice(0, 6), transcript().slice(6)]) {
        store.getState().applySubscribed({ type: 'subscribed', session, transcript: page, logs: [],
            latest: page.at(-1).hub_sequence, replay_more: page.at(-1).sequence < 12 });
    }
    verify(roundsFor(store));
});

it('does not change A when queued inputs arrive after its execution has started', () => {
    const store = storeWithInputs(['A']);
    for (const event of transcript()) {
        store.getState().applyEvent({ type: 'event', session: 'demo', hub_seq: event.hub_sequence,
            envelope: event });
        if (event.sequence === 2) {
            addInput(store, 'B');
            addInput(store, 'C');
        }
    }
    verify(roundsFor(store));
});

it('uses the rejected data ID when an older envelope still echoes the active run', () => {
    const store = storeWithInputs();
    for (const event of transcript()) {
        const delivered = event.event === 'input_rejected'
            ? { ...event, request_id: 'A', run_id: 'run-A' } : event;
        store.getState().applyEvent({ type: 'event', session: 'demo', hub_seq: delivered.hub_sequence,
            envelope: delivered });
    }
    verify(roundsFor(store));
});

it('reconstructs A, B, and the rejected C after reload with no retained outboxes or request records', () => {
    const store = createPanelStore();
    store.getState().applySubscribed({ type: 'subscribed', session, transcript: transcript(), logs: [], latest: 12 });
    verify(roundsFor(store), true);
});

it('uses run IDs for delayed results and scopes reused identities to the worker', () => {
    const events = [
        envelope(1, 'input_admitted', 'A', { operation: 'message' }),
        envelope(2, 'model_response', 'A', response('Answer A', 'same-call')),
        envelope(3, 'run_finished', 'A', { status: 'completed' }),
        envelope(4, 'input_admitted', 'B', { operation: 'message' }),
        envelope(5, 'model_response', 'B', response('Answer B', 'same-call')),
        // The result belongs to A even though B is now the current run.
        envelope(6, 'tool_results', 'A', [result('same-call', 'Output A')], { request_id: '' }),
        envelope(7, 'run_finished', 'B', { status: 'cancelled' }),
        envelope(8, 'input_admitted', 'A', { operation: 'message' }, { worker_id: 'worker-2' }),
        envelope(9, 'model_response', 'A', response('Replacement answer', 'same-call'), { worker_id: 'worker-2' }),
        envelope(10, 'tool_results', 'A', [result('same-call', 'Replacement output')], { worker_id: 'worker-2' }),
        envelope(11, 'run_finished', 'A', { status: 'failed' }, { worker_id: 'worker-2' }),
    ];
    const rounds = buildRounds(events.map((event) => ({ kind: 'event', id: `event-${event.sequence}`,
        epoch: 'epoch-1', envelope: event })), new Map());
    assert.equal(rounds.length, 3);
    assert.equal(rounds[0].calls[0].result.text, 'Output A');
    assert.equal(rounds[1].calls[0].result, null);
    assert.equal(rounds[1].calls[0].status, 'unknown');
    assert.equal(rounds[1].status, 'cancelled');
    assert.equal(rounds[2].calls[0].result.text, 'Replacement output');
    assert.equal(rounds[2].assistant[0].text, 'Replacement answer');
    assert.equal(rounds[2].status, 'failed');
});

function crossPanelTranscript() {
    return [
        envelope(1, 'input_admitted', 'A', { operation: 'message' }),
        envelope(2, 'run_started', 'A', {}),
        envelope(3, 'input_committed', 'A', {}),
        envelope(4, 'model_response', 'A', response('Answer A', 'call-A')),
        envelope(5, 'tool_results', 'A', [result('call-A', 'Output A')]),
        envelope(6, 'run_finished', 'A', { status: 'completed' }),
        envelope(7, 'input_admitted', 'B', { operation: 'message' }),
        envelope(8, 'run_started', 'B', {}),
        envelope(9, 'input_committed', 'B', {}),
        envelope(10, 'model_response', 'B', response('Answer B', 'call-B')),
        envelope(11, 'tool_results', 'B', [result('call-B', 'Output B')]),
        envelope(12, 'run_finished', 'B', { status: 'failed', exchanges: 1,
            error: 'HTTP 503 after retries', failure: { stage: 'model_request', can_continue: true } }),
    ];
}

for (const replay of [false, true]) {
    it(`orders a remote A before locally queued B through ${replay ? 'reconnect replay' : 'live delivery'}, history refresh and reload`, () => {
        // A was sent by another panel. Our B outbox exists before A's admission
        // reaches this panel, so draft creation and execution orders differ.
        const store = storeWithInputs(['B']);
        const events = crossPanelTranscript();
        if (replay) {
            for (const page of [events.slice(0, 6), events.slice(6)]) {
                store.getState().applySubscribed({ type: 'subscribed', session,
                    transcript: page, logs: [], latest: page.at(-1).hub_sequence,
                    replay_more: page.at(-1).sequence < 12 });
            }
        } else {
            for (const event of events) {
                store.getState().applyEvent({ type: 'event', session: 'demo',
                    hub_seq: event.hub_sequence, envelope: event });
            }
        }
        const history = ['A', 'B'].map((id, index) => ({ index,
            user: [{ type: 'text', modality: 'text', raw: id }],
            steps: [{ index: 0, content: [{ type: 'text', modality: 'text', raw: `Answer ${id}` }],
                tool_calls: 1 }], omitted_steps: 0,
        }));
        const checkOrder = (panel) => {
            const runs = roundsFor(panel).filter((round) => round.kind === 'run');
            assert.deepEqual(runs.map((round) => round.index), [1, 2]);
            assert.deepEqual(runs.map((round) => round.assistant[0].text), ['Answer A', 'Answer B']);
            assert.deepEqual(runs.map((round) => round.calls[0].result.text), ['Output A', 'Output B']);
            assert.equal(runs[0].admitted.envelope.request_id, 'A');
            assert.equal(runs.at(-1).status, 'failed');
            assert.equal(runs.at(-1).failure.canContinue, true);
            return runs;
        };
        checkOrder(store);
        // Simulate both the in-place refresh and a fresh page with no outbox.
        const reloaded = createPanelStore();
        reloaded.getState().applySubscribed({ type: 'subscribed', session,
            transcript: events, logs: [], latest: 12 });
        for (const panel of [store, reloaded]) {
            panel.getState().beginHistory('demo');
            assert.equal(panel.getState().applyHistoryPage('demo',
                envelope(13, 'history', 'history', {}, { run_id: '' }), {
                    request_id: 'history', start: 0, step: 0, next: 2, next_step: 0,
                    revision: 1, total: 2, turns: history,
                }), true);
            const runs = checkOrder(panel);
            const view = panel.getState().views.get('demo');
            assert.equal(view.historyLoading, false);
            assert.deepEqual(view.history.map((turn) => turn.user[0].raw), ['A', 'B']);
            assert.equal(runs[1].input?.requestId ?? runs[1].admitted.envelope.request_id, 'B');
        }
    });
}

it('keeps automatic compact progress in the original run on live and replay paths', () => {
    const events = [
        envelope(1, 'input_admitted', 'A', { operation: 'message' }),
        envelope(2, 'run_started', 'A', {}),
        envelope(3, 'input_committed', 'A', {}),
        envelope(4, 'tool_calls', 'A', [{ id: 'host-1', name: 'auto_compact', arguments: {} }]),
        envelope(5, 'compact_finished', 'A', { origin: 'automatic', summary: 'handoff',
            memory_file: '/memory/1/state.md', revision: 3, removed_turns: 1, durable: true }),
        envelope(6, 'tool_results', 'A', [result('host-1', 'Context compacted')]),
        envelope(7, 'model_response', 'A', { ...response('continued answer', 'next'), commit_sequence: '3' }),
        envelope(8, 'run_finished', 'A', { status: 'completed', exchanges: 3 }),
    ];
    for (const replay of [false, true]) {
        const store = storeWithInputs(replay ? [] : ['A']);
        if (replay) store.getState().applySubscribed({ type: 'subscribed', session,
            transcript: events, logs: [], latest: 8, replay_more: false });
        else for (const event of events) store.getState().applyEvent({ type: 'event', session: 'demo',
            hub_seq: event.sequence, envelope: event });
        const rounds = roundsFor(store).filter(round => round.kind === 'run');
        assert.equal(rounds.length, 1);
        assert.equal(rounds[0].compacting, false);
        assert.equal(rounds[0].compactResult, null);
        assert.equal(rounds[0].timeline.some(item => item.kind === 'compact'), false);
        assert.equal(rounds[0].calls[0].result.text, 'Context compacted');
        assert.equal(rounds[0].assistant[0].text, 'continued answer');
        assert.equal(rounds[0].status, 'completed');
    }
});

it('shows failed and cancelled host compaction cards with their actual outcomes', () => {
    for (const status of ['failed', 'cancelled']) {
        const store = storeWithInputs(['A']);
        const returned = result('host-1', `Compaction ${status}`);
        returned.invoke_return.extras = { status,
            ...(status === 'failed' ? { error: { stage: 'auto_compact', message: 'summary failed' } } : {}) };
        const events = [envelope(1, 'input_admitted', 'A', { operation: 'message' }),
            envelope(2, 'tool_calls', 'A', [{ id: 'host-1', name: 'auto_compact', arguments: {} }]),
            envelope(3, 'tool_results', 'A', [returned]),
            envelope(4, 'run_finished', 'A', { status, error: 'summary failed',
                failure: { stage: 'model_request', operation: 'auto_compact', can_continue: true } })];
        for (const event of events) store.getState().applyEvent({ type: 'event', session: 'demo',
            hub_seq: event.sequence, envelope: event });
        const run = roundsFor(store).find(round => round.kind === 'run');
        assert.equal(run.calls[0].status, status);
        if (status === 'failed') assert.equal(run.failure.operation, 'auto_compact');
    }
});
