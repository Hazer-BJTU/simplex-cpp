/**
 * @file panel store: the four behaviours that are different on purpose.
 *
 * `web/src/state/store.ts` is the port of the old panel's `state.js`, and these
 * tests cover exactly the places the port deliberately diverges — each of them
 * a defect the old panel shipped. They run under `node --test` rather than in a
 * browser because the store is the *vanilla* Zustand store: no React, no DOM,
 * so a rule about replay merging can be checked in milliseconds.
 *
 * Every test here was written against the behaviour of the panel it replaced,
 * and fails against it — which is what makes these regression tests rather than
 * descriptions. That panel is gone (P8); the behaviour it had is not, and these
 * are now the only record of what was wrong with it that is executable.
 */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { createPanelStore, statsFor } from '../web/src/state/store.ts';

/** One worker envelope as the hub forwards it. */
function envelope(hubSequence, event = 'model_response', extra = {}) {
    return {
        type: 'event',
        event,
        session_id: 'demo',
        worker_id: 'worker-1',
        request_id: 'req-1',
        run_id: 'run-1',
        sequence: hubSequence,
        data: {},
        hub_sequence: hubSequence,
        received_at: '2026-01-01T00:00:00.000Z',
        ...extra,
    };
}

/** One session description as `/api/sessions` and `welcome` report it. */
function session(id = 'demo', extra = {}) {
    return {
        session_id: id,
        created_at: '2026-01-01T00:00:00.000Z',
        spec: {},
        connected: true,
        identity: { state: 'idle', worker_id: null, since: null },
        stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 0 },
        last_run_id: '',
        last_event_at: null,
        last_event: null,
        confirmations: [],
        process: null,
        requests: [],
        ...extra,
    };
}

/** Hub metadata, with the epoch the tests care about. */
function meta(epoch) {
    return {
        name: 'simplex-hub',
        version: '0.1.0',
        protocol: { name: 'simplex-hub-panel', version: 1 },
        worker_protocol: '1',
        capabilities: ['transcript-replay', 'transcript-epoch', 'global-confirmations'],
        ...(epoch === undefined ? {} : { transcript_epoch: epoch }),
        listen: { host: '127.0.0.1', port: 8800 },
        launcher: { kind: 'local', owns_config: true },
        provider_profiles: [],
        force_kill_process_group: false,
        mock: { enabled: true },
    };
}

function welcome(epoch, sessions) {
    return { type: 'welcome', hub: meta(epoch), sessions, subscriptions: [] };
}

function subscribed(sessionId, transcript, extra = {}) {
    return {
        type: 'subscribed',
        session: session(sessionId),
        transcript,
        logs: [],
        latest: transcript.at(-1)?.hub_sequence ?? 0,
        ...extra,
    };
}

/** The transcript items that are worker envelopes, for length assertions. */
function events(store, id = 'demo') {
    return store.getState().items(id).filter((item) => item.kind === 'event');
}

function notes(store, id = 'demo') {
    return store.getState().items(id).filter((item) => item.kind === 'note');
}

describe('worker-backed display history', () => {
    it('assembles pages and keeps query bodies out of the event transcript', () => {
        const store = createPanelStore();
        store.getState().beginHistory('demo');
        const first = envelope(1, 'history', { data: {
            request_id: 'h-1', revision: 1, start: 0, step: 0,
            next: 1, next_step: 0, total: 2,
            turns: [{ index: 0, user: [{ type: 'text', raw: 'old input' }],
                steps: [], omitted_steps: 0 }],
        } });
        store.getState().applyEvent({ type: 'event', session: 'demo', hub_seq: 1,
            envelope: first });
        store.getState().applyHistoryPage('demo', first);
        assert.equal(store.getState().view('demo').historyLoading, true);
        assert.equal(events(store).length, 0);
        const second = envelope(2, 'history', { data: {
            request_id: 'h-2', revision: 1, start: 1, step: 0,
            next: 2, next_step: 0, total: 2,
            turns: [{ index: 1, user: [{ type: 'text', raw: 'new input' }],
                steps: [], omitted_steps: 0 }],
        } });
        store.getState().applyHistoryPage('demo', second);
        const view = store.getState().view('demo');
        assert.equal(view.history.length, 2);
        assert.equal(view.historyLoading, false);
        assert.equal(view.historySequence, 2);
    });

    it('joins two pages of one long turn without duplicating its user input', () => {
        const store = createPanelStore();
        store.getState().beginHistory('demo');
        const page = (sequence, step, nextStep, text) => envelope(sequence, 'history', {
            data: { request_id: `h-${sequence}`, revision: 1, start: 0, step,
                next: nextStep ? 0 : 1, next_step: nextStep, total: 1,
                turns: [{ index: 0, user: [{ type: 'text', raw: 'input' }],
                    steps: [{ index: step, content: [{ type: 'text', raw: text }],
                        tool_calls: 0 }], omitted_steps: nextStep ? 1 : 0 }],
            },
        });
        store.getState().applyHistoryPage('demo', page(1, 0, 1, 'first'));
        store.getState().applyHistoryPage('demo', page(2, 1, 0, 'second'));
        const history = store.getState().view('demo').history;
        assert.equal(history.length, 1);
        assert.deepEqual(history[0].steps.map((step) => step.content[0].raw),
            ['first', 'second']);
        assert.equal(store.getState().view('demo').historyLoading, false);
    });
});

describe('panel store: replay is merged, not substituted (A2)', () => {
    it('keeps the transcript a reconnect did not re-send', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));

        store.getState().applySubscribed(subscribed('demo', [envelope(1), envelope(2)]));
        assert.equal(events(store).length, 2);

        // The hub replays only what came after the panel's cursor — which is
        // exactly what `state.js` used to throw the rest away for.
        store.getState().applySubscribed(subscribed('demo', [envelope(3)]));

        assert.equal(events(store).length, 3, 'the older envelopes were discarded');
        assert.equal(store.getState().lastSeq('demo'), 3);
    });

    it('counts a replayed range it already holds as duplicates instead of repeating it', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        store.getState().applySubscribed(subscribed('demo', [envelope(1), envelope(2)]));
        // The same range again, as a second overlapping subscribe would send.
        store.getState().applySubscribed(subscribed('demo', [envelope(1), envelope(2)]));

        assert.equal(events(store).length, 2);
        assert.equal(statsFor(store.getState(), 'demo').duplicates, 2);
    });

    it('marks a gap when the hub ring no longer reaches back to the cursor', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        // The worker's own sequence stays continuous across the eviction, so the
        // only gap detector that can fire here is the replay one.
        store.getState().applySubscribed(subscribed('demo', [
            envelope(1, 'model_response', { sequence: 1 }),
            envelope(2, 'model_response', { sequence: 2 }),
        ]));
        // Envelopes 3..7 were evicted before this replay could ask for them.
        store.getState().applySubscribed(subscribed('demo', [
            envelope(8, 'model_response', { sequence: 3 }),
        ]));

        const warnings = notes(store);
        assert.equal(warnings.length, 1);
        assert.match(warnings[0].text, /transcript gap/);
        assert.match(warnings[0].text, /resumed at hub_sequence 8/);
        assert.equal(statsFor(store.getState(), 'demo').gaps, 1);
    });

    it('counts a jump in the worker\'s own numbering, which the hub cannot see', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        store.getState().applySubscribed(subscribed('demo', [
            envelope(1, 'model_response', { sequence: 1 }),
            // The hub's numbering is contiguous; the worker skipped 2.
            envelope(2, 'model_response', { sequence: 3 }),
        ]));

        assert.equal(statsFor(store.getState(), 'demo').gaps, 1);
        assert.equal(notes(store).length, 0, 'the hub replayed what it had; nothing was evicted');
    });
});

describe('panel store: a hub restart invalidates the cursor', () => {
    it('resets the cursor and says so when the epoch changes', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        store.getState().applySubscribed(subscribed('demo', [envelope(1), envelope(2)]));

        // A new hub process: same session, numbering starts again at 1.
        store.getState().applyWelcome(welcome('epoch-2', [session()]));

        assert.equal(store.getState().lastSeq('demo'), 0, 'the stale cursor survived');
        assert.equal(events(store).length, 2, 'the visible history was thrown away');
        assert.match(notes(store)[0].text, /hub restarted/);

        // Replaying the new process's numbering appends rather than colliding
        // with the envelope that already had sequence 1.
        store.getState().applySubscribed(subscribed('demo', [envelope(1)]));
        assert.equal(events(store).length, 3);
        assert.equal(store.getState().lastSeq('demo'), 1);
    });

    it('refuses a delta numbered against an epoch it was not holding', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        store.getState().applySubscribed(subscribed('demo', [envelope(1)]));

        // The answer carries the epoch the cursor did *not* belong to, so the
        // delta describes a series the panel is not holding.
        const effects = store.getState().applySubscribed(
            subscribed('demo', [envelope(2)], { transcript_epoch: 'epoch-2' }),
        );

        assert.deepEqual(effects.resubscribe, { session: 'demo', since: 0 });
        assert.equal(events(store).length, 1, 'the unusable delta was merged anyway');
        assert.equal(store.getState().lastSeq('demo'), 0);
    });

    it('spots a restart from a counter that went backwards, with no epoch to consult', () => {
        const store = createPanelStore();
        // An older hub: it reports no epoch at all.
        store.getState().applyWelcome(welcome(undefined, [session()]));
        store.getState().applySubscribed(subscribed('demo', [envelope(1), envelope(2), envelope(3)]));

        // Its counter starts over, so `latest` is behind the panel's cursor.
        const effects = store.getState().applySubscribed(
            subscribed('demo', [], { latest: 1 }),
        );

        assert.deepEqual(effects.resubscribe, { session: 'demo', since: 0 });
        assert.equal(store.getState().lastSeq('demo'), 0);
        assert.match(notes(store)[0].text, /went backwards/);
        assert.equal(events(store).length, 3, 'the visible history was thrown away');
    });

    it('leaves the cursor alone when the epoch is unchanged', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        store.getState().applySubscribed(subscribed('demo', [envelope(1), envelope(2)]));
        store.getState().applyWelcome(welcome('epoch-1', [session()]));

        assert.equal(store.getState().lastSeq('demo'), 2);
        assert.equal(notes(store).length, 0);
    });
});

describe('panel store: a session list never deletes (D23)', () => {
    it('merges a list that answers a refresh', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session('a'), session('b')]));

        // A REST refresh that was in flight while `c` was created. It does not
        // mention `c`, and the old store deleted whatever a list omitted.
        store.getState().upsertSessions([session('a')]);

        assert.deepEqual([...store.getState().sessions.keys()].sort(), ['a', 'b']);
    });

    it('replaces the list on welcome, which nothing can have raced', () => {
        const store = createPanelStore();
        store.getState().upsertSessions([session('ghost')]);
        store.getState().applyWelcome(welcome('epoch-1', [session('a')]));

        assert.deepEqual([...store.getState().sessions.keys()], ['a']);
    });

    it('drops a selection the hub no longer lists', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session('a')]));
        store.getState().setSelected('a');
        store.getState().applyWelcome(welcome('epoch-1', [session('b')]));

        assert.equal(store.getState().selected, null);
    });

    it('removes a session when the hub says so', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session('a')]));
        store.getState().removeSession('a');

        assert.equal(store.getState().sessions.size, 0);
        assert.equal(store.getState().view('a').items.length, 0);
    });
});

describe('panel store: the log pane is a tail (D20)', () => {
    it('replaces the lines rather than appending them', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));

        store.getState().applyLogs({ type: 'logs', session: 'demo', lines: ['one', 'two'], dropped: 0 });
        // A refresh returns the same tail again; concatenating doubled the pane.
        store.getState().applyLogs({ type: 'logs', session: 'demo', lines: ['one', 'two'], dropped: 0 });

        assert.deepEqual([...store.getState().logs('demo').lines], ['one', 'two']);
    });

    it('keeps a bounded window of a longer tail', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        const lines = Array.from({ length: 1200 }, (_, index) => `line ${index}`);
        store.getState().applyLogs({ type: 'logs', session: 'demo', lines, dropped: 3 });

        assert.equal(store.getState().logs('demo').lines.length, 500);
        assert.equal(store.getState().logs('demo').lines.at(-1), 'line 1199');
        assert.equal(store.getState().logs('demo').dropped, 3);
    });
});

describe('panel store: a refused input is handed back (D19)', () => {
    it('returns the text to the composer and takes the message off the transcript', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));

        store.getState().beginInput('demo', 'req-9', [{ type: 'text', raw: 'hello' }], 'message');
        assert.equal(store.getState().items('demo').length, 1);

        store.getState().applyError({
            type: 'error',
            error: 'input_not_sent',
            message: 'the worker is not connected',
            session: 'demo',
            request: { type: 'input', session: 'demo', request_id: 'req-9' },
        });

        assert.equal(store.getState().items('demo').length, 0);
        const failed = store.getState().failedInput;
        assert.equal(failed.parts[0].raw, 'hello');
        assert.match(failed.reason, /not connected/);

        store.getState().clearFailedInput();
        assert.equal(store.getState().failedInput, null);
    });

    it('keeps the text on screen once the worker admits it', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        store.getState().beginInput('demo', 'req-9', [{ type: 'text', raw: 'hello' }], 'message');

        store.getState().applyEvent({
            type: 'event',
            session: 'demo',
            hub_seq: 1,
            envelope: envelope(1, 'input_admitted', { request_id: 'req-9' }),
        });

        const item = store.getState().items('demo')[0];
        assert.equal(item.kind, 'outbox');
        assert.equal(item.state, 'admitted');
        // The worker protocol sends `input_admitted` with an empty payload, so
        // this item is the only place the operator's own words exist.
        assert.equal(item.parts[0].raw, 'hello');
    });

    it('does not hand back an input the hub accepted', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        store.getState().beginInput('demo', 'req-9', [{ type: 'text', raw: 'hello' }], 'message');
        store.getState().applyAccepted({
            type: 'accepted', action: 'input', session: 'demo', request_id: 'req-9',
        });

        assert.equal(store.getState().failedInput, null);
        assert.equal(store.getState().items('demo').length, 1);
    });

    it('adds no second row for an input it is already showing', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        store.getState().beginInput('demo', 'req-9', [{ type: 'text', raw: 'hello' }], 'message');
        store.getState().applyEvent({
            type: 'event',
            session: 'demo',
            hub_seq: 1,
            envelope: envelope(1, 'input_admitted', { request_id: 'req-9' }),
        });

        // The outbox item is the input; `input_admitted` carries no payload, so
        // a separate placeholder for it would just repeat the same message.
        assert.equal(store.getState().items('demo').length, 1);
    });

    it('shows a placeholder for an input this page never sent', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));
        // Replayed history: no outbox item exists, so the admission is the only
        // trace of the operator's turn and must not vanish.
        store.getState().applySubscribed(subscribed('demo', [
            envelope(1, 'input_admitted', { request_id: 'someone-elses' }),
        ]));

        assert.equal(store.getState().items('demo').length, 1);
        assert.equal(store.getState().items('demo')[0].kind, 'event');
    });
});

describe('panel store: an open prompt comes from the session description', () => {
    const prompt = {
        confirmation_id: 'c-1',
        session_id: 'demo',
        worker_id: 'worker-1',
        run_id: 'run-1',
        state: 'awaiting-decision',
        verified: true,
        identity_state: 'trusted',
        call: { name: 'run_command', arguments: { command: 'ls' } },
        received_at: '2026-01-01T00:00:00.000Z',
        deadline_at: null,
        settled_at: null,
        decision: null,
        reason: null,
    };

    it('survives a subscribe, which `subscribed` alone never managed', () => {
        const store = createPanelStore();
        // The description is the only place the hub puts an open prompt: the
        // old panel read a `confirmations` field off `subscribed`, which the
        // hub has never sent, so after a reload nothing showed a prompt that
        // was still waiting for an answer.
        store.getState().applyWelcome(welcome('epoch-1', [session('demo', { confirmations: [prompt] })]));
        store.getState().applySubscribed(subscribed('demo', [], {
            session: session('demo', { confirmations: [prompt] }),
        }));

        assert.equal(store.getState().openConfirmations('demo').length, 1);
    });

    it('drops a prompt the newest description no longer lists', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session('demo', { confirmations: [prompt] })]));
        store.getState().applySubscribed(subscribed('demo', [], {
            session: session('demo', { confirmations: [prompt] }),
        }));

        // Settled while the panel was away: a description is a snapshot of what
        // is open *now*, so a button that can no longer do anything must go.
        store.getState().applySubscribed(subscribed('demo', []));
        assert.equal(store.getState().openConfirmations('demo').length, 0);
    });

    it('surfaces a prompt for a session the panel is not watching', () => {
        // This is what the hub's `global-confirmations` capability means: the
        // approval is not stranded behind a session the operator is not looking
        // at. The prompt is the first thing the panel ever hears about `other`.
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session('demo'), session('other')]));
        store.getState().setSelected('demo');
        store.getState().applyConfirmation({
            type: 'confirmation', session: 'other', open: true, confirmation: prompt,
        });

        assert.equal(store.getState().openConfirmations('other').length, 1);
        assert.equal(store.getState().openConfirmations('demo').length, 0);
    });
});

describe('panel store: capabilities are read, not assumed', () => {
    it('reports what the hub advertised', () => {
        const store = createPanelStore();
        store.getState().applyWelcome(welcome('epoch-1', [session()]));

        assert.equal(store.getState().hasCapability('transcript-epoch'), true);
        assert.equal(store.getState().hasCapability('supervisor'), false);
    });
});
