/** Aggregate deltas must preserve eviction/replay and immediate global approval visibility. */
import assert from 'node:assert/strict';
import { it } from 'node:test';
import { createPanelStore } from '../web/src/state/store.ts';
import { emptyView, noteItem, viewDisplayBytes } from '../web/src/state/view.ts';
import { viewProjection } from '../web/src/state/viewProjection.ts';

function event(session, sequence, name = 'model_response', data = {}) {
    return { type: 'event', session_id: session, event: name, worker_id: 'worker',
        sequence, hub_sequence: sequence, run_id: 'run', request_id: 'request', data };
}
function prompt(session, id, received = '2026-01-01', extras = {}) {
    return { session_id: session, confirmation_id: id, worker_id: 'worker', run_id: 'run',
        received_at: received, settled_at: null, call: { name: 'run_command', arguments: {} }, ...extras };
}
function session(id, confirmations = []) {
    return { session_id: id, connected: true, created_at: '', spec: {}, requests: [], confirmations,
        identity: { state: 'live', worker_id: 'worker', since: null } };
}
function confirm(store, approval, open = true) {
    store.getState().applyConfirmation({ type: 'confirmation', session: approval.session_id,
        confirmation: approval, open });
}
/** Full uncached serialization, independent of the production measurement caches. */
function referenceBytes(views) {
    let bytes = 0;
    for (const view of views.values()) {
        for (const collection of [view.items, view.history, view.historyLoad?.history ?? [],
            view.transcriptNotices, Object.values(view.latestEvents)]) {
            for (const item of collection) bytes += Buffer.byteLength(JSON.stringify(item));
        }
    }
    return bytes;
}
function exact(store) {
    assert.equal(viewProjection(store.getState().views).bytes, referenceBytes(store.getState().views));
}

it('does not remeasure inactive views or rescan unchanged latest-event records during a live burst', () => {
    const store = createPanelStore();
    const views = new Map([['active', emptyView('active')]]);
    let inactiveReads = 0;
    for (let index = 0; index < 80; index += 1) {
        const view = { ...emptyView(`inactive-${index}`), items: [noteItem('retained '.repeat(100))] };
        views.set(view.id, new Proxy(view, { get(target, key) {
            if (['items', 'history', 'historyLoad', 'transcriptNotices', 'latestEvents', 'confirmations'].includes(key)) {
                inactiveReads += 1;
            }
            return Reflect.get(target, key);
        } }));
    }
    store.setState({ views, selected: 'active' });
    viewProjection(views); // Index a bulk/external snapshot once.
    inactiveReads = 0;
    const approvals = viewProjection(views).prompts;
    for (let index = 1; index <= 200; index += 1) {
        store.getState().applyEvent({ session: 'active', envelope: event('active', index) });
        store.getState().applyLogs({ session: 'active', lines: ['log'], dropped: 0 });
        assert.equal(viewProjection(store.getState().views).prompts, approvals);
    }
    assert.equal(inactiveReads, 0, 'ordinary event deltas never inspect inactive display/approval fields');
    exact(store);

    let enumerations = 0;
    const latestEvents = new Proxy({ status: event('latest', 1, 'status') }, {
        ownKeys(target) { enumerations += 1; return Reflect.ownKeys(target); },
    });
    const view = { ...emptyView('latest'), latestEvents };
    const bytes = viewDisplayBytes(view);
    enumerations = 0;
    for (let index = 0; index < 100; index += 1) {
        assert.equal(viewDisplayBytes({ ...view, logs: { lines: [`log ${index}`], dropped: 0 } }), bytes);
    }
    assert.equal(enumerations, 0, 'unchanged latest-event records keep their measured size');
});

it('accounts for replay, snapshot/history candidates, epoch replacement, deletion and unknown external maps', () => {
    const store = createPanelStore();
    for (const id of ['a', 'b', 'c']) {
        store.getState().mergeTranscript(id, [event(id, 1), event(id, 2)], 2);
        exact(store);
    }
    const current = store.getState().view('b');
    store.setState({ views: new Map(store.getState().views).set('b', {
        ...current, history: [{ index: 0, user: [], steps: [] }],
        historyLoad: { history: [{ index: 1, user: [{ type: 'text', raw: 'candidate' }], steps: [] }] },
    }) });
    store.getState().applyLogs({ session: 'a', lines: ['update after external replacement'] });
    exact(store);
    store.getState().applySnapshot({ type: 'snapshot', session: session('b'), transcript: [event('b', 3)] });
    exact(store);
    store.setState({ epoch: 'old' });
    store.getState().applyWelcome({ type: 'welcome', hub: { transcript_epoch: 'new', capabilities: [] },
        sessions: ['a', 'b', 'c'].map(id => session(id)), subscriptions: [] });
    exact(store);
    store.getState().removeSession('b');
    exact(store);
    store.getState().removeSession('absent');
    exact(store);
    store.getState().applyEvent({ session: 'new', envelope: event('new', 1) });
    exact(store);
});

it('keeps deltas exact through both eviction passes and preserves selected/control/replay/approval semantics', () => {
    const store = createPanelStore();
    store.getState().setSelected('selected');
    store.getState().applyEvent({ session: 'selected', envelope: event('selected', 1) });
    const approval = prompt('inactive', 'still-open');
    confirm(store, approval);
    const prompts = viewProjection(store.getState().views).prompts;
    store.getState().applyEvent({ session: 'inactive', envelope: event('inactive', 1, 'tool_calls', []) });
    for (let index = 0; index < 24; index += 1) {
        const id = `control-${index}`;
        store.getState().applyEvent({ session: id,
            envelope: event(id, 1, 'status', { raw: 'x'.repeat(2 * 1024 * 1024) }) });
        exact(store);
        assert.equal(viewProjection(store.getState().views).prompts, prompts);
        assert.ok(viewProjection(store.getState().views).bytes <= 40 * 1024 * 1024);
    }
    const inactive = store.getState().view('inactive');
    assert.equal(inactive.replayRequired, true);
    assert.equal(inactive.lastSeq, 0);
    assert.equal(inactive.confirmations.get('still-open'), approval);
    assert.ok(store.getState().view('selected').items.length > 0);
    assert.ok([...store.getState().views.values()].some(view => view.replayRequired && !view.latestEvents.status));
});

it('publishes changed approval previews and settlement synchronously, preserving stable tie ordering', () => {
    const store = createPanelStore();
    // Create the first view before its later-arriving prompt, preserving the original view order on timestamp ties.
    store.getState().applyLogs({ session: 'first', lines: [] });
    const second = prompt('second', 'second');
    const first = prompt('first', 'first');
    confirm(store, second);
    confirm(store, first);
    let projection = viewProjection(store.getState().views);
    assert.deepEqual(projection.prompts, [first, second]);
    store.getState().applyLogs({ session: 'unvisited', lines: ['no approvals'] });
    assert.equal(viewProjection(store.getState().views).prompts, projection.prompts);
    const richer = { ...first, call: { ...first.call, arguments: { command: 'complete preview' } } };
    let observed;
    const unsubscribe = store.subscribe(state => { observed = viewProjection(state.views).prompts; });
    confirm(store, richer);
    assert.deepEqual(observed, [richer, second], 'subscribers see new previews in the same update');
    confirm(store, richer, false);
    assert.deepEqual(observed, [second]);
    const settled = prompt('first', 'settled', '2025', { settled_at: 'now' });
    projection = viewProjection(store.getState().views);
    confirm(store, settled);
    assert.equal(viewProjection(store.getState().views).prompts, projection.prompts);
    store.getState().removeSession('second');
    assert.deepEqual(observed, []);
    unsubscribe();
    exact(store);
});

it('refreshes global approvals from welcome/subscription snapshots while retaining unchanged prompt arrays', () => {
    const store = createPanelStore();
    const first = prompt('a', 'first');
    store.getState().applyWelcome({ type: 'welcome', sessions: [session('a', [first])], subscriptions: [] });
    const before = viewProjection(store.getState().views).prompts;
    store.getState().applyWelcome({ type: 'welcome', sessions: [session('a', [first])], subscriptions: [] });
    assert.equal(viewProjection(store.getState().views).prompts, before);
    const richer = { ...first, arguments_truncated: false,
        call: { ...first.call, arguments: { command: 'restored full arguments' } } };
    store.getState().applySubscribed({ type: 'subscribed', session: session('a', [richer]),
        transcript: [], latest: 0, logs: [] });
    assert.equal(viewProjection(store.getState().views).prompts[0].call.arguments.command, 'restored full arguments');
    store.getState().applyWelcome({ type: 'welcome', sessions: [session('a')], subscriptions: [] });
    assert.deepEqual(viewProjection(store.getState().views).prompts, []);
    exact(store);
});
