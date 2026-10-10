/** Recovery state must outlive the bounded display copies it describes. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { createPanelStore } from '../web/src/state/store.ts';
import {
    LATEST_EVENT_BYTE_CAP, LATEST_EVENT_CAP, TRANSCRIPT_CAP,
    TRANSCRIPT_NOTICE_BYTE_CAP, TRANSCRIPT_NOTICE_CAP,
    addTranscriptNotice, displayBytes, emptyView, indexEnvelope, noteItem, viewDisplayBytes,
} from '../web/src/state/view.ts';

function envelope(sequence, event = 'model_response', data = {}) {
    return { type: 'event', event, session_id: 'demo', worker_id: 'worker',
        sequence, hub_sequence: sequence, run_id: 'run', request_id: 'input', data };
}

function session(requests = []) {
    return { session_id: 'demo', created_at: '', spec: {}, connected: true,
        identity: { state: 'live', worker_id: 'worker', since: null },
        confirmations: [], requests, last_run_id: 'run' };
}

function request(state = 'sent') {
    return { request_id: 'query', operation: 'status', state, sent_at: '', observed_at: null, detail: state };
}

/** Exercise the normal aggregate eviction path rather than injecting an evicted view. */
function pressure(store) {
    const raw = 'x'.repeat(5 * 1024 * 1024);
    for (let index = 0; index < 9; ++index) {
        const id = `other-${index}`;
        store.getState().applyEvent({ session: id,
            envelope: { ...envelope(1, 'model_response', { content: [{ type: 'text', raw }] }),
                session_id: id } });
    }
}

it('eviction resets replay/index state and reselect rebuilds request chips and tool events', () => {
    const store = createPanelStore();
    store.getState().upsertSession(session([request()]));
    store.getState().setSelected('demo');
    const status = envelope(1, 'status', { active: false });
    const events = [status, envelope(2, 'input_admitted', { operation: 'message' }),
        envelope(3, 'tool_calls', [{ id: 'call', name: 'run_command', arguments: {} }]),
        envelope(4, 'run_finished', { status: 'completed' })];
    store.getState().applySubscribed({ type: 'subscribed', session: session([request()]),
        transcript: events, latest: 4, logs: [] });
    const prompt = { confirmation_id: 'approval', worker_id: 'worker', run_id: 'run',
        received_at: '', settled_at: null, call: { id: 'call', name: 'run_command', arguments: {} } };
    store.getState().applyConfirmation({ type: 'confirmation', session: 'demo', confirmation: prompt, open: true });
    assert.equal(store.getState().view('demo').requestIndex.size, 1);
    assert.equal(store.getState().view('demo').seenRequests.has('input'), true);
    store.getState().setSelected('other-0');
    pressure(store);

    const evicted = store.getState().view('demo');
    assert.equal(evicted.lastSeq, 0);
    assert.equal(evicted.replayRequired, true);
    assert.equal(evicted.requestIndex.size, 0);
    assert.equal(evicted.seenRequests.size, 0);
    assert.deepEqual(evicted.lastSequenceByWorker, {});
    assert.deepEqual(evicted.items, []);
    assert.equal(evicted.latestEvents.status, status);
    assert.equal(evicted.confirmations.get('approval'), prompt);
    assert.match(evicted.transcriptNotices[0].text, /replays what the Hub still retains/);
    assert.ok([...store.getState().views.values()].reduce((sum, view) => sum + viewDisplayBytes(view), 0)
        < 40 * 1024 * 1024);

    // A frame already in flight must not turn the empty replay cursor into a
    // cursor that skips the discarded prefix (legacy hubs have no reset flag).
    store.getState().applyEvent({ session: 'demo', envelope: events.at(-1) });
    assert.equal(store.getState().lastSeq('demo'), 4);
    assert.equal(store.getState().view('demo').replayRequired, true);
    store.getState().setSelected('demo');
    store.getState().applySubscribed({ type: 'subscribed', session: session([request('observed')]),
        transcript: events, latest: 4, logs: [] });
    const recovered = store.getState().view('demo');
    assert.deepEqual(recovered.items.filter(item => item.kind === 'event')
        .map(item => item.envelope.hub_sequence), [1, 2, 3, 4]);
    assert.equal(recovered.items.filter(item => item.kind === 'request').length, 1);
    assert.equal(recovered.requestIndex.get('query').request.state, 'observed');
    assert.equal(recovered.lastSeq, 4);
    assert.equal(recovered.replayRequired, false);
    assert.equal(recovered.gaps, 0);
});

it('successive request updates keep the chip index pointing at the current item', () => {
    const store = createPanelStore();
    for (const state of ['sent', 'unknown', 'observed']) {
        store.getState().applyRequest({ type: 'request', session: 'demo', request: request(state) });
        const view = store.getState().view('demo');
        assert.equal(view.items.length, 1);
        assert.equal(view.items[0].request.state, state);
        assert.equal(view.requestIndex.get('query'), view.items[0]);
    }
    store.getState().mergeTranscript('demo', Array.from({ length: TRANSCRIPT_CAP }, (_, index) =>
        envelope(index + 1)), TRANSCRIPT_CAP);
    assert.equal(store.getState().view('demo').requestIndex.size, 0,
        'trimming an updated chip must also remove its index');
});

it('control priority does not defeat the aggregate inactive-display budget', () => {
    const store = createPanelStore();
    store.getState().setSelected('demo');
    for (let index = 0; index < 24; ++index) {
        const id = `control-${index}`;
        store.getState().applyEvent({ session: id,
            envelope: { ...envelope(1, 'status', { raw: 'x'.repeat(2 * 1024 * 1024) }), session_id: id } });
    }
    const views = [...store.getState().views.values()];
    assert.ok(views.reduce((sum, view) => sum + viewDisplayBytes(view), 0) <= 40 * 1024 * 1024);
    const evicted = views.find(view => view.replayRequired && !view.latestEvents.status);
    assert.ok(evicted, 'large inactive control caches must remain evictable');
    assert.equal(evicted.lastSeq, 0);
    assert.ok(store.getState().view('control-23').latestEvents.status);
});

it('gap and restart warnings survive count/byte trimming and transcript replacement', () => {
    const store = createPanelStore();
    store.getState().setSelected('demo');
    store.getState().mergeTranscript('demo', Array.from({ length: TRANSCRIPT_CAP }, (_, index) =>
        envelope(index + 1)), TRANSCRIPT_CAP);
    store.getState().mergeTranscript('demo', [envelope(TRANSCRIPT_CAP + 2)], TRANSCRIPT_CAP + 2);
    assert.equal(store.getState().items('demo').length, TRANSCRIPT_CAP);
    assert.match(store.getState().view('demo').transcriptNotices[0].text, /transcript gap/);
    store.getState().applyEvent({ session: 'demo', envelope: envelope(TRANSCRIPT_CAP + 3,
        'model_response', { content: [{ type: 'text', raw: 'x'.repeat(9 * 1024 * 1024) }] }) });
    assert.equal(store.getState().items('demo').length, 0);
    assert.equal(store.getState().view('demo').transcriptNotices.length, 1);
    store.setState({ epoch: 'old' });
    store.getState().applyWelcome({ type: 'welcome', hub: { transcript_epoch: 'new', capabilities: [] },
        sessions: [session()], subscriptions: [] });
    assert.match(store.getState().view('demo').transcriptNotices.at(-1).text, /hub restarted/);
    const notices = store.getState().view('demo').transcriptNotices;
    store.getState().applySnapshot({ type: 'snapshot', session: session(), transcript: [] });
    assert.equal(store.getState().view('demo').transcriptNotices, notices);
});

it('recovery notices stay bounded and initial replay explicitly reports unavailable events', () => {
    const store = createPanelStore();
    store.getState().setSelected('demo');
    for (let index = 0; index < 20; ++index) {
        store.getState().mergeTranscript('demo', [{ ...envelope((index + 1) * 10),
            sequence: index + 1 }], (index + 1) * 10);
    }
    const view = store.getState().view('demo');
    assert.equal(view.transcriptNotices.length, TRANSCRIPT_NOTICE_CAP);
    assert.ok(displayBytes(view.transcriptNotices) <= TRANSCRIPT_NOTICE_BYTE_CAP);
    assert.equal(view.gaps, 20);
    let large = addTranscriptNotice(emptyView('large'), noteItem('x'.repeat(12000), 'warn'));
    const latest = noteItem('y'.repeat(12000), 'warn');
    large = addTranscriptNotice(large, latest);
    assert.deepEqual(large.transcriptNotices, [latest]);
    assert.ok(displayBytes(large.transcriptNotices) <= TRANSCRIPT_NOTICE_BYTE_CAP);
});

it('snapshot replacement preserves truncated worker history; successful compact invalidates it', () => {
    const store = createPanelStore();
    const history = [{ index: 4, user: [], steps: [], omitted_steps: 0 }];
    store.setState({ views: new Map([['demo', { ...emptyView('demo'), history,
        historyTruncated: true, historySequence: 5, historyWorker: 'worker' }]]) });
    store.getState().applySnapshot({ type: 'snapshot', session: session(), transcript: [envelope(1)] });
    assert.equal(store.getState().view('demo').history, history);
    assert.equal(store.getState().view('demo').historyTruncated, true);
    store.getState().applySnapshot({ type: 'snapshot', session: session(), transcript: [
        envelope(6, 'compact_finished', { summary: 'saved', memory_file: '/memory/archive.md',
            removed_turns: 5, revision: 2, durable: true }),
    ] });
    assert.deepEqual(store.getState().view('demo').history, []);
    assert.equal(store.getState().view('demo').historyTruncated, false);
});

it('large output and unknown names do not evict control caches; all cache bounds still hold', () => {
    const controls = [envelope(1, 'ready'), envelope(2, 'status'), envelope(3, 'options'),
        envelope(4, 'compact_finished')];
    let view = emptyView('demo');
    for (const event of controls) view = indexEnvelope(view, event);
    for (let index = 5; index < 55; ++index) view = indexEnvelope(view, envelope(index, `unknown-${index}`));
    view = indexEnvelope(view, envelope(55, 'model_response', { raw: 'x'.repeat(LATEST_EVENT_BYTE_CAP) }));
    for (const event of controls) assert.equal(view.latestEvents[event.event], event);
    assert.equal(view.latestEvents.model_response, undefined);
    assert.ok(Object.keys(view.latestEvents).length <= LATEST_EVENT_CAP);
    assert.ok(displayBytes(Object.values(view.latestEvents)) <= LATEST_EVENT_BYTE_CAP);

    // Recency, not first insertion, decides which incidental key is evicted.
    let recent = emptyView('recent');
    for (let index = 1; index <= LATEST_EVENT_CAP; ++index) {
        recent = indexEnvelope(recent, envelope(index, `unknown-${index}`));
    }
    const update = envelope(33, 'unknown-1');
    recent = indexEnvelope(recent, update);
    recent = indexEnvelope(recent, envelope(34, 'unknown-33'));
    assert.equal(recent.latestEvents['unknown-1'], update);
    assert.equal(recent.latestEvents['unknown-2'], undefined);
    view = indexEnvelope(view, envelope(90, 'status', { raw: 'x'.repeat(LATEST_EVENT_BYTE_CAP) }));
    assert.ok(displayBytes(Object.values(view.latestEvents)) <= LATEST_EVENT_BYTE_CAP,
        'control priority must not bypass the hard byte budget');
});
