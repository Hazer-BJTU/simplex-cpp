/**
 * @file client-side panel store.
 *
 * Holds everything the panel knows: hub metadata, panel-socket state, the
 * session list, and one bounded view per session (transcript items, tracked
 * requests, open confirmations, worker logs, the latest event of each name).
 *
 * The store never touches the DOM; it reports changes through `subscribe`
 * callbacks so the view layer can update incrementally instead of re-rendering
 * the transcript on every message. Transcript items are either
 * `{kind:'event', envelope}`, `{kind:'request', request}` or
 * `{kind:'note', text, tone}`.
 */

/** Envelopes retained per session on the client. */
export const TRANSCRIPT_CAP = 2000;

/** Worker log lines retained for the log pane. */
export const LOG_CAP = 500;

/** Tracked request outcomes retained per session. */
export const REQUEST_CAP = 200;

/** Event names the hub groups into runs (mirrors hub/src/protocol/events.js). */
export const RUN_START_EVENTS = new Set(['input_admitted', 'run_started']);
export const RUN_END_EVENTS = new Set(['run_finished']);

/** Fresh per-session view state. */
function emptyView(id) {
    return {
        id,
        items: [],
        /** Highest hub_sequence observed; also the replay cursor. */
        lastSeq: 0,
        /** request_id -> {kind:'request'} item currently in `items`. */
        requestIndex: new Map(),
        /** request_id -> hub request entry. */
        requests: new Map(),
        /** confirmation_id -> prompt. */
        confirmations: new Map(),
        logs: { lines: [], dropped: 0, logPath: null },
        /** event name -> most recent envelope. */
        latestEvents: Object.create(null),
        runActive: false,
        lastRunId: '',
        gaps: 0,
        duplicates: 0,
        droppedItems: 0,
        lastOutcome: null,
        /** worker_id -> last worker sequence seen, for gap detection. */
        lastSequenceByWorker: Object.create(null),
    };
}

/** Coerce a transcript payload into ordered, deduplicated event items. */
function eventItems(transcript) {
    const items = [];
    if (!Array.isArray(transcript)) return items;
    for (const envelope of transcript) {
        if (envelope && typeof envelope === 'object') items.push({ kind: 'event', envelope });
    }
    return items;
}

/**
 * Fold one envelope into a view's derived caches (latest event per name, run
 * activity, last run id, worker-sequence gap counting).
 *
 * Used for live envelopes and for replayed ones, so a freshly subscribed panel
 * shows the Status/Options panes without waiting for new events.
 */
function indexEnvelope(st, envelope) {
    const name = typeof envelope.event === 'string' ? envelope.event : '';
    if (name) st.latestEvents[name] = envelope;
    if (typeof envelope.run_id === 'string' && envelope.run_id.length > 0) {
        st.lastRunId = envelope.run_id;
    }
    if (RUN_START_EVENTS.has(name)) st.runActive = true;
    else if (RUN_END_EVENTS.has(name)) st.runActive = false;
    else if (name === 'status' || name === 'ready') {
        const data = envelope.data ?? {};
        if (typeof data.active === 'boolean') st.runActive = data.active;
        if (data.loop && typeof data.loop.status === 'string') {
            st.runActive = data.loop.status === 'running' || data.active === true;
        }
    }
    const workerId = typeof envelope.worker_id === 'string' ? envelope.worker_id : '';
    if (typeof envelope.sequence === 'number') {
        const previous = st.lastSequenceByWorker[workerId];
        if (typeof previous === 'number' && envelope.sequence !== previous + 1) st.gaps += 1;
        st.lastSequenceByWorker[workerId] = envelope.sequence;
    }
}

/**
 * Create a store.
 *
 * @param {object} [options]
 * @param {number} [options.transcriptCap]
 * @param {number} [options.logCap]
 * @param {number} [options.requestCap]
 */
export function createStore({
    transcriptCap = TRANSCRIPT_CAP,
    logCap = LOG_CAP,
    requestCap = REQUEST_CAP,
} = {}) {
    const listeners = new Set();
    const sessions = new Map();
    const views = new Map();
    let hub = null;
    let panel = { state: 'idle', attempt: 0, nextDelayMs: null, authRequired: false, error: null };
    let selected = null;

    function emit(change) {
        for (const listener of [...listeners]) {
            try {
                listener(change);
            } catch (error) {
                // A broken view must not stop the store from reporting to others.
                globalThis.console?.error?.('panel store listener failed', error);
            }
        }
    }

    function view(sessionId) {
        let existing = views.get(sessionId);
        if (!existing) {
            existing = emptyView(sessionId);
            views.set(sessionId, existing);
        }
        return existing;
    }

    /** Trim a view's transcript, keeping the request index consistent. */
    function trim(st) {
        if (st.items.length <= transcriptCap) return;
        const removed = st.items.splice(0, st.items.length - transcriptCap);
        st.droppedItems += removed.length;
        for (const item of removed) {
            if (item.kind === 'request') {
                const current = st.requestIndex.get(item.request.request_id);
                if (current === item) st.requestIndex.delete(item.request.request_id);
            }
        }
    }

    /** Reset a view's transcript and replay it from a hub-provided list. */
    function replaceTranscript(sessionId, transcript) {
        const st = view(sessionId);
        const items = eventItems(transcript);
        st.items = items;
        st.requestIndex = new Map();
        st.droppedItems = 0;
        st.latestEvents = Object.create(null);
        st.lastSequenceByWorker = Object.create(null);
        st.runActive = false;
        st.lastRunId = '';
        st.gaps = 0;
        // Requests already represented by an admission/rejection envelope must
        // not also appear as a trailing seeded chip.
        st.seenRequests = new Set();
        let maxSeq = 0;
        for (const item of items) {
            const envelope = item.envelope;
            indexEnvelope(st, envelope);
            const seq = envelope?.hub_sequence;
            if (typeof seq === 'number' && seq > maxSeq) maxSeq = seq;
            if (envelope?.event === 'input_admitted' && typeof envelope.request_id === 'string') {
                st.seenRequests.add(envelope.request_id);
            }
            if (envelope?.event === 'input_rejected') {
                const rejected = envelope.data?.request_id;
                if (typeof rejected === 'string') st.seenRequests.add(rejected);
            }
        }
        st.lastSeq = maxSeq;
    }

    /** Rebuild request chips and latest-event cache after a transcript reset. */
    function seedFromSession(sessionId) {
        const st = view(sessionId);
        const session = sessions.get(sessionId);
        if (!session) return;
        for (const entry of Array.isArray(session.requests) ? session.requests : []) {
            if (!entry || typeof entry.request_id !== 'string') continue;
            st.requests.set(entry.request_id, entry);
            if (st.requestIndex.has(entry.request_id)) continue;
            if (st.seenRequests?.has(entry.request_id)) continue;
            const item = { kind: 'request', request: entry };
            st.items.push(item);
            st.requestIndex.set(entry.request_id, item);
        }
        if (typeof session.last_run_id === 'string' && session.last_run_id) {
            st.lastRunId = session.last_run_id;
        }
    }

    const store = {
        // ------------------------------------------------------------ wiring --
        subscribe(listener) {
            listeners.add(listener);
            return () => listeners.delete(listener);
        },

        // ------------------------------------------------------------- reads --
        hub: () => hub,
        panel: () => panel,
        selected: () => selected,
        get: (sessionId) => sessions.get(sessionId) ?? null,
        list: () => [...sessions.values()],
        ids: () => [...sessions.keys()],
        view,
        lastSeq: (sessionId) => views.get(sessionId)?.lastSeq ?? 0,
        items: (sessionId) => views.get(sessionId)?.items ?? [],
        logs: (sessionId) => views.get(sessionId)?.logs ?? { lines: [], dropped: 0, logPath: null },
        latestEvent(sessionId, name) {
            const st = views.get(sessionId);
            return st ? st.latestEvents[name] ?? null : null;
        },
        latestData(sessionId, name) {
            const envelope = this.latestEvent(sessionId, name);
            return envelope?.data ?? null;
        },
        openConfirmations(sessionId) {
            const st = views.get(sessionId);
            return st ? [...st.confirmations.values()] : [];
        },
        confirmation(sessionId, confirmationId) {
            return views.get(sessionId)?.confirmations.get(confirmationId) ?? null;
        },
        requests(sessionId) {
            const st = views.get(sessionId);
            return st ? [...st.requests.values()] : [];
        },
        unknownRequests(sessionId) {
            return this.requests(sessionId).filter((entry) => entry.state === 'unknown');
        },
        isRunActive(sessionId) {
            return Boolean(views.get(sessionId)?.runActive);
        },
        lastRunId(sessionId) {
            const st = views.get(sessionId);
            if (st?.lastRunId) return st.lastRunId;
            return sessions.get(sessionId)?.last_run_id ?? '';
        },
        /** Loop status/phase from the latest `status` snapshot, if any. */
        loop(sessionId) {
            const data = this.latestData(sessionId, 'status') ?? this.latestData(sessionId, 'ready');
            const loop = data?.loop;
            return loop && typeof loop === 'object' ? loop : null;
        },
        statusData(sessionId) {
            return this.latestData(sessionId, 'status') ?? this.latestData(sessionId, 'ready');
        },
        /** Model in effect: spec override, else the latest options snapshot. */
        model(sessionId) {
            const session = sessions.get(sessionId);
            if (!session) return '';
            if (typeof session.spec?.model === 'string' && session.spec.model.length > 0) {
                return session.spec.model;
            }
            const current = this.latestData(sessionId, 'options')?.model?.current;
            if (current && typeof current.model === 'string') return current.model;
            if (typeof session.spec?.provider === 'string') return session.spec.provider;
            return '';
        },
        stats(sessionId) {
            const st = views.get(sessionId);
            return {
                items: st?.items.length ?? 0,
                lastSeq: st?.lastSeq ?? 0,
                gaps: st?.gaps ?? 0,
                duplicates: st?.duplicates ?? 0,
                droppedItems: st?.droppedItems ?? 0,
                confirmations: st?.confirmations.size ?? 0,
                unknownRequests: st ? [...st.requests.values()]
                    .filter((entry) => entry.state === 'unknown').length : 0,
            };
        },

        // ------------------------------------------------------------ writes --
        setHub(nextHub) {
            hub = nextHub ?? null;
            emit({ type: 'hub' });
        },
        setPanel(patch) {
            panel = { ...panel, ...patch };
            emit({ type: 'panel' });
        },
        setSelected(sessionId) {
            if (selected === sessionId) return;
            selected = sessionId;
            emit({ type: 'selected', sessionId });
        },
        setSessions(list) {
            const seen = new Set();
            for (const session of Array.isArray(list) ? list : []) {
                if (!session || typeof session.session_id !== 'string') continue;
                seen.add(session.session_id);
                sessions.set(session.session_id, session);
            }
            for (const id of [...sessions.keys()]) {
                if (!seen.has(id)) sessions.delete(id);
            }
            emit({ type: 'sessions' });
        },
        upsertSession(session) {
            if (!session || typeof session.session_id !== 'string') return;
            sessions.set(session.session_id, session);
            emit({ type: 'session', sessionId: session.session_id });
        },
        applyWelcome(message) {
            if (message.hub) hub = message.hub;
            store.setSessions(message.sessions);
            emit({ type: 'hub' });
        },
        removeSession(sessionId) {
            sessions.delete(sessionId);
            views.delete(sessionId);
            if (selected === sessionId) {
                selected = null;
                emit({ type: 'selected', sessionId: null });
            }
            emit({ type: 'session-removed', sessionId });
        },
        applySubscribed(message) {
            const sessionId = message.session?.session_id ?? message.session;
            if (typeof sessionId !== 'string') return;
            if (message.session) sessions.set(sessionId, message.session);
            const st = view(sessionId);
            const previousLast = st.lastSeq;
            replaceTranscript(sessionId, message.transcript);
            seedFromSession(sessionId);
            const latest = typeof message.latest === 'number' ? message.latest : 0;
            const firstSeq = st.items.find((item) => item.kind === 'event')?.envelope?.hub_sequence;
            if (typeof firstSeq === 'number' && previousLast > 0 && firstSeq > previousLast + 1) {
                // The hub's ring no longer holds everything the panel asked for.
                st.items.unshift({
                    kind: 'note',
                    tone: 'warn',
                    text: `transcript gap: replay resumed at hub_sequence ${firstSeq}`
                        + ` (last seen ${previousLast}); earlier envelopes are gone`,
                });
                st.gaps += 1;
            }
            st.lastSeq = Math.max(st.lastSeq, latest);
            if (Array.isArray(message.logs)) {
                st.logs.lines = message.logs.slice(-logCap);
                emit({ type: 'logs', sessionId });
            }
            if (Array.isArray(message.confirmations)) {
                for (const prompt of message.confirmations) {
                    if (prompt?.confirmation_id) st.confirmations.set(prompt.confirmation_id, prompt);
                }
            }
            emit({ type: 'subscribed', sessionId });
            emit({ type: 'session', sessionId });
        },
        applyEvent(message) {
            const sessionId = message.session ?? message.envelope?.session_id;
            const envelope = message.envelope;
            if (typeof sessionId !== 'string' || !envelope || typeof envelope !== 'object') return;
            const st = view(sessionId);
            const seq = typeof envelope.hub_sequence === 'number' ? envelope.hub_sequence : null;
            if (seq !== null && st.lastSeq > 0 && seq <= st.lastSeq) {
                st.duplicates += 1;
                return;
            }
            if (seq !== null) st.lastSeq = seq;

            // A worker's own sequence numbers are the gap signal; hub_sequence
            // only orders what this hub process saw.
            indexEnvelope(st, envelope);

            const item = { kind: 'event', envelope };
            st.items.push(item);
            trim(st);
            emit({ type: 'event', sessionId, envelope, item });
            const name = envelope.event;
            if (name === 'status' || name === 'ready' || name === 'options') {
                emit({ type: 'snapshot-event', sessionId, name, data: envelope.data });
            }
        },
        applyRequest(message) {
            const sessionId = message.session;
            const entry = message.request;
            if (typeof sessionId !== 'string' || !entry || typeof entry.request_id !== 'string') return;
            const st = view(sessionId);
            st.requests.set(entry.request_id, entry);
            if (st.requests.size > requestCap) {
                for (const [key, value] of st.requests) {
                    if (st.requests.size <= requestCap) break;
                    if (value.state === 'sent') continue;
                    st.requests.delete(key);
                }
            }
            const existing = st.requestIndex.get(entry.request_id);
            if (existing) {
                existing.request = entry;
                emit({ type: 'request-update', sessionId, request: entry, item: existing });
                return;
            }
            const item = { kind: 'request', request: entry };
            st.items.push(item);
            st.requestIndex.set(entry.request_id, item);
            trim(st);
            emit({ type: 'request', sessionId, request: entry, item });
        },
        applyConfirmation(message) {
            const sessionId = message.session;
            const prompt = message.confirmation;
            if (typeof sessionId !== 'string' || !prompt?.confirmation_id) return;
            const st = view(sessionId);
            if (message.open) {
                st.confirmations.set(prompt.confirmation_id, prompt);
                emit({ type: 'confirmation-open', sessionId, prompt });
                return;
            }
            st.confirmations.delete(prompt.confirmation_id);
            st.lastOutcome = {
                sessionId,
                confirmationId: prompt.confirmation_id,
                outcome: message.outcome ?? null,
                prompt,
            };
            emit({
                type: 'confirmation-closed',
                sessionId,
                prompt,
                outcome: message.outcome ?? null,
            });
        },
        applyProcess(message) {
            const sessionId = message.session;
            if (typeof sessionId !== 'string') return;
            const session = sessions.get(sessionId);
            if (session) session.process = message.process ?? null;
            emit({ type: 'process', sessionId, process: message.process ?? null });
        },
        applyConnection(message) {
            const sessionId = message.session;
            if (typeof sessionId !== 'string') return;
            const session = sessions.get(sessionId);
            if (session) {
                session.connected = Boolean(message.connected);
                if (message.identity) {
                    session.identity = {
                        ...(session.identity ?? {}),
                        ...message.identity,
                    };
                }
            }
            emit({ type: 'connection', sessionId, connected: Boolean(message.connected) });
        },
        applyLogs(message) {
            const sessionId = message.session;
            if (typeof sessionId !== 'string') return;
            const st = view(sessionId);
            const incoming = Array.isArray(message.lines) ? message.lines : [];
            if (incoming.length > 0) {
                const lines = st.logs.lines.concat(incoming);
                st.logs.lines = lines.length > logCap ? lines.slice(-logCap) : lines;
            }
            if (typeof message.dropped === 'number') st.logs.dropped = message.dropped;
            if (typeof message.log_path === 'string') st.logs.logPath = message.log_path;
            emit({ type: 'logs', sessionId });
        },
        applySnapshot(message) {
            const sessionId = message.session?.session_id ?? message.session;
            if (typeof sessionId !== 'string') return;
            if (message.session && typeof message.session === 'object') {
                sessions.set(sessionId, message.session);
            }
            replaceTranscript(sessionId, message.transcript);
            seedFromSession(sessionId);
            emit({ type: 'snapshot', sessionId });
        },
        /** Append a synthetic note to a transcript (replay gaps, local warnings). */
        note(sessionId, text, tone = 'muted') {
            const st = view(sessionId);
            const item = { kind: 'note', text, tone };
            st.items.push(item);
            trim(st);
            emit({ type: 'note', sessionId, item });
        },
    };

    return store;
}
