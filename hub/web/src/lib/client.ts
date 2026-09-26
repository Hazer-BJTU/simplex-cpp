/**
 * @file the panel's client: it owns the transport and feeds the store.
 *
 * This is the module that knows a socket exists. The store does not, the
 * components do not, and that is the point — the previous panel spread this
 * wiring across `app.js`, `api.js` and `state.js`, which is why a rule like
 * "`subscribed` replaces the transcript" could hide in one file while the
 * cursor that made it wrong was set in another.
 *
 * Everything a command can return is deliberately narrow. `sendInput` reports
 * whether the frame left the machine, never whether the worker acted on it: the
 * request's real state arrives later as a `request` message, and the panel's
 * job is to keep saying `sent` until it does.
 */
import type {
    ContentPart,
    HubMessage,
    PayloadOptions,
    SessionId,
    SessionSpec,
} from '../../../shared/protocol.ts';
import type { PanelStoreApi } from '../state/store.ts';
import { panelStore } from '../state/store.ts';
import { ApiError, createRest, type RestClient, type WorkerAction } from './rest.ts';
import { createPanelSocket, type PanelSocket } from './socket.ts';
import { createTokenStore, type HistoryLike, type KeyValueStorage, type LocationLike, type TokenStore } from './token.ts';

/** Query parameter carrying the selected session, so a view can be linked to. */
export const SESSION_PARAM = 'session';

/** Everything `createPanelClient` reads. */
export interface PanelClientOptions {
    store?: PanelStoreApi;
    location?: LocationLike | null;
    history?: HistoryLike | null;
    storage?: KeyValueStorage;
    fetchImpl?: typeof fetch;
    WebSocketImpl?: typeof WebSocket;
}

/** The panel's connection to one hub. */
export interface PanelClient {
    readonly tokens: TokenStore;
    readonly rest: RestClient;
    readonly store: PanelStoreApi;
    /** Read the URL, fetch metadata, and connect. */
    start(): Promise<void>;
    stop(): void;
    /** Select a session: subscribe to it, unsubscribe from the previous one. */
    select(sessionId: SessionId | null): void;
    /** Ask for a session's transcript and live events. */
    subscribe(sessionId: SessionId, since?: number): void;
    /** Ask the hub to re-send a session's whole transcript, replacing what is held. */
    reloadTranscript(sessionId: SessionId): void;
    /** Refresh the worker-backed display history. */
    reloadHistory(sessionId: SessionId): boolean;
    refreshSessions(): void;
    /**
     * Send a message.
     *
     * `options.confirmation.mode` is carried on every payload rather than held
     * on the hub, because it is the panel's setting: the worker freezes the
     * policy per run, so sending it each time is what keeps a change take
     * effect on the next run instead of the next restart.
     */
    sendInput(
        sessionId: SessionId,
        parts: readonly ContentPart[],
        operation?: string,
        options?: PayloadOptions,
    ): boolean;
    /** Ask the hub for a session's captured worker output. */
    refreshLogs(sessionId: SessionId, limit?: number): boolean;
    /** Time a round trip to the hub; the result lands in `pingMs`. */
    ping(): boolean;
    /** Read the worker's persisted snapshot, and keep it only if still current. */
    loadSnapshot(sessionId: SessionId): Promise<void>;
    sendSignal(
        sessionId: SessionId,
        operation: 'status' | 'options' | 'cancel' | 'shutdown',
        runId?: string,
    ): boolean;
    sendConfirmation(
        sessionId: SessionId,
        confirmationId: string,
        decision: 'approved' | 'denied',
        reason?: string,
    ): boolean;
    workerAction(sessionId: SessionId, action: WorkerAction, spec?: SessionSpec): Promise<void>;
    createSession(sessionId: SessionId, spec?: SessionSpec): Promise<boolean>;
    deleteSession(sessionId: SessionId): Promise<boolean>;
    /** Replace the panel token and reconnect with it. */
    setToken(value: string): Promise<void>;
}

/** A fresh request id. `crypto.randomUUID` needs a secure context. */
function newRequestId(): string {
    const random = globalThis.crypto?.randomUUID?.();
    if (random) return `panel-${random}`;
    return `panel-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

/** The session named in the URL, or null. */
function sessionFromUrl(location: LocationLike | null): SessionId | null {
    try {
        const value = location?.search
            ? new URLSearchParams(location.search).get(SESSION_PARAM)
            : null;
        return value ? value : null;
    } catch {
        return null;
    }
}

/**
 * Rewrite the address bar so the current view can be linked to (defect D26:
 * the old panel read `?session=` and never wrote it, so a reload lost it).
 */
function writeSessionToUrl(
    sessionId: SessionId | null,
    location: LocationLike | null,
    history: HistoryLike | null,
): void {
    try {
        if (!location || !history) return;
        const url = new URL(location.href);
        const current = url.searchParams.get(SESSION_PARAM);
        if (sessionId === null && current === null) return;
        if (sessionId !== null && current === sessionId) return;
        if (sessionId === null) url.searchParams.delete(SESSION_PARAM);
        else url.searchParams.set(SESSION_PARAM, sessionId);
        history.replaceState(null, '', `${url.pathname}${url.search}${url.hash}`);
    } catch {
        // A failed rewrite costs a link, never the session.
    }
}

export function createPanelClient(options: PanelClientOptions = {}): PanelClient {
    const store = options.store ?? panelStore;
    const loc = options.location ?? (globalThis.location as LocationLike | undefined) ?? null;
    const hist = options.history ?? (globalThis.history as HistoryLike | undefined) ?? null;
    const tokens = createTokenStore({
        location: loc,
        history: hist,
        ...(options.storage ? { storage: options.storage } : {}),
    });

    const restOptions = {
        token: () => tokens.get(),
        onUnauthorized: () => {
            tokens.clear();
            store.getState().setAuthRequired(true);
        },
    } as const;
    const rest = createRest(options.fetchImpl
        ? { ...restOptions, fetchImpl: options.fetchImpl }
        : restOptions);

    const socket: PanelSocket = createPanelSocket({
        location: loc,
        token: () => tokens.get(),
        onState: (status) => store.getState().setConnection(status),
        onEvent: (event) => {
            if (event.kind === 'ignored') store.getState().noteIgnoredFrame();
            else if (event.kind === 'refused') store.getState().noteRefusal(event.code, event.detail);
            else handleMessage(event.message);
        },
        ...(options.WebSocketImpl ? { WebSocketImpl: options.WebSocketImpl } : {}),
    });

    /**
     * The cursor to replay from.
     *
     * A hub that does not advertise `transcript-replay` has never promised that
     * `since` means anything, so the panel asks for everything rather than
     * trusting a number that hub may not honour.
     */
    function cursorFor(sessionId: SessionId): number {
        if (!store.getState().hasCapability('transcript-replay')) return 0;
        return store.getState().lastSeq(sessionId);
    }

    function subscribe(sessionId: SessionId, since?: number): void {
        const cursor = since ?? cursorFor(sessionId);
        socket.send({ type: 'subscribe', session: sessionId, since: cursor });
    }

    const historyRequests = new Map<SessionId, {
        id: string; start: number; step: number;
        revision: number | null; retries: number;
    }>();

    /** The hub can route queries, but only a worker that advertises them can answer. */
    function workerSupportsHistory(sessionId: SessionId): boolean {
        const state = store.getState();
        const session = state.sessions.get(sessionId);
        const workerId = session?.identity.worker_id;
        const events = state.views.get(sessionId)?.latestEvents;
        if (!session?.connected || !workerId || !events) return false;
        for (const name of ['status', 'ready']) {
            const envelope = events[name];
            if (envelope?.worker_id !== workerId) continue;
            const data = envelope.data as { capabilities?: unknown } | null;
            if (Array.isArray(data?.capabilities)
                && data.capabilities.includes('session-history')) return true;
        }
        return false;
    }

    function requestHistory(sessionId: SessionId, start = 0, step = 0,
        revision: number | null = null, retries = 0): boolean {
        if (!store.getState().hasCapability('session-history')
            || !workerSupportsHistory(sessionId)) return false;
        const requestId = newRequestId();
        const sent = socket.send({ type: 'history', session: sessionId,
            request_id: requestId, start, step, limit: 10 });
        if (sent) {
            historyRequests.set(sessionId, {
                id: requestId, start, step, revision, retries,
            });
            if (start === 0 && step === 0) store.getState().beginHistory(sessionId);
        }
        return sent;
    }

    /** When the outstanding heartbeat was sent, so its reply can be timed. */
    let pendingPing = 0;

    function handleMessage(message: HubMessage): void {
        switch (message.type) {
            case 'welcome': {
                store.getState().applyWelcome(message);
                // Subscribing after `applyWelcome` is what makes a restarted
                // hub replay from the start: the epoch reset happens there.
                const selected = store.getState().selected;
                if (selected) subscribe(selected);
                return;
            }
            case 'subscribed': {
                const effects = store.getState().applySubscribed(message);
                if (effects.resubscribe) {
                    subscribe(effects.resubscribe.session, effects.resubscribe.since);
                } else if (message.session.connected) {
                    requestHistory(message.session.session_id);
                }
                return;
            }
            case 'sessions':
                // A reply to `list_sessions`, racing with any REST refresh.
                // Merged, never destructive (defect D23).
                store.getState().upsertSessions(message.sessions);
                return;
            case 'session':
                store.getState().upsertSession(message.session);
                return;
            case 'created':
                store.getState().upsertSession(message.session);
                store.getState().setSelected(message.session.session_id);
                writeSessionToUrl(message.session.session_id, loc, hist);
                subscribe(message.session.session_id);
                return;
            case 'session_removed': {
                const wasSelected = store.getState().selected === message.session;
                store.getState().removeSession(message.session);
                if (wasSelected) writeSessionToUrl(null, loc, hist);
                return;
            }
            case 'event':
                store.getState().applyEvent(message);
                if ((message.envelope.event === 'ready' || message.envelope.event === 'status')
                    && store.getState().selected === message.session
                    && !historyRequests.has(message.session)
                    && store.getState().views.get(message.session)?.historyWorker
                        !== message.envelope.worker_id) {
                    requestHistory(message.session);
                }
                if (message.envelope.event === 'history') {
                    const page = message.envelope.data as {
                        request_id?: unknown; next?: unknown; next_step?: unknown;
                        total?: unknown;
                        revision?: unknown;
                    } | null;
                    const pending = historyRequests.get(message.session);
                    if (page && pending && pending.id === page.request_id) {
                        const nextStep = typeof page.next_step === 'number'
                            ? page.next_step : 0;
                        if (typeof page.next !== 'number'
                            || typeof page.total !== 'number'
                            || typeof page.next_step !== 'number'
                            || page.next < pending.start
                            || page.next === pending.start && nextStep <= pending.step
                                && page.next < page.total) {
                            historyRequests.delete(message.session);
                            store.getState().endHistory(message.session);
                            store.getState().setNotice('error', 'history_cursor',
                                'Worker returned a history page without a progressing cursor.');
                            return;
                        }
                        if (pending.revision !== null && page.revision !== pending.revision) {
                            if (pending.retries < 3) {
                                requestHistory(message.session, 0, 0, null, pending.retries + 1);
                            } else {
                                historyRequests.delete(message.session);
                                store.getState().endHistory(message.session);
                                store.getState().setNotice('warn', 'history_changed',
                                    'Conversation changed during history loading; refresh after the run settles.');
                            }
                            return;
                        }
                        store.getState().applyHistoryPage(message.session, message.envelope);
                        if (typeof page.next === 'number' && typeof page.total === 'number'
                            && (page.next < page.total
                                || typeof page.next_step === 'number' && page.next_step > 0)) {
                            requestHistory(message.session, page.next,
                                nextStep,
                                typeof page.revision === 'number' ? page.revision : null,
                                pending.retries);
                        } else {
                            historyRequests.delete(message.session);
                        }
                    }
                } else if (message.envelope.event === 'run_finished'
                    && store.getState().selected === message.session) {
                    requestHistory(message.session);
                } else if (message.envelope.event === 'history_error') {
                    const data = message.envelope.data as { request_id?: unknown } | null;
                    if (historyRequests.get(message.session)?.id === data?.request_id) {
                        historyRequests.delete(message.session);
                        store.getState().endHistory(message.session);
                    }
                }
                return;
            case 'request':
                store.getState().applyRequest(message);
                return;
            case 'confirmation':
                store.getState().applyConfirmation(message);
                return;
            case 'process':
                store.getState().applyProcess(message);
                return;
            case 'connection':
                store.getState().applyConnection(message);
                if (message.connected && store.getState().selected === message.session) {
                    requestHistory(message.session);
                } else if (!message.connected) {
                    historyRequests.delete(message.session);
                    store.getState().endHistory(message.session);
                }
                return;
            case 'logs':
                store.getState().applyLogs(message);
                return;
            case 'snapshot':
                store.getState().applySnapshot(message);
                return;
            case 'accepted':
                store.getState().applyAccepted(message);
                return;
            case 'error':
                store.getState().applyError(message);
                if (typeof message.request === 'object' && message.request !== null
                    && (message.request as { type?: unknown }).type === 'history') {
                    const session = (message.request as { session?: unknown }).session;
                    if (typeof session === 'string') {
                        historyRequests.delete(session);
                        store.getState().endHistory(session);
                    }
                }
                return;
            case 'pong':
                store.getState().finishPing(pendingPing, Date.now());
                return;
            default:
                // Unreachable while `HubMessage` is exhaustive, and the reason
                // `checkHubEnvelope` exists: a newer hub's message type is
                // counted and dropped rather than crashing the dispatch.
                store.getState().noteIgnoredFrame();
                return;
        }
    }

    /** Report a REST failure, distinguishing a rejected token from the rest. */
    function reportApiError(error: unknown, what: string): void {
        if (error instanceof ApiError) {
            if (error.unauthorized) {
                store.getState().setAuthRequired(true);
                return;
            }
            store.getState().setNotice('error', error.code, `${what}: ${error.message}`);
            return;
        }
        const detail = error instanceof Error ? error.message : String(error);
        store.getState().setNotice('error', 'internal_error', `${what}: ${detail}`);
    }

    return {
        tokens,
        rest,
        store,

        async start() {
            // REST first: unlike a browser WebSocket, it reports 401 unambiguously.
            try {
                store.getState().setHub(await rest.meta());
            } catch (error) {
                reportApiError(error, 'hub metadata unavailable');
            }
            try {
                store.getState().upsertSessions((await rest.sessions()).sessions);
            } catch (error) {
                reportApiError(error, 'session list unavailable');
            }
            // A deep link is only honoured when the hub actually lists it;
            // `welcome` would otherwise clear the selection a moment later.
            const wanted = sessionFromUrl(loc);
            if (wanted && store.getState().sessions.has(wanted)) {
                store.getState().setSelected(wanted);
            }
            socket.connect();
        },

        stop() {
            socket.close();
        },

        select(sessionId) {
            const previous = store.getState().selected;
            if (previous === sessionId) return;
            store.getState().setSelected(sessionId);
            writeSessionToUrl(sessionId, loc, hist);
            if (previous) socket.send({ type: 'unsubscribe', session: previous });
            if (sessionId) subscribe(sessionId);
        },

        subscribe,

        reloadTranscript(sessionId) {
            // A snapshot is the whole transcript, so the store replaces rather
            // than merges. This is also what makes `status_snapshot` — a
            // message the old panel never sent, leaving it with no way back
            // from a lost transcript — a working recovery path.
            const sent = socket.send({ type: 'status_snapshot', session: sessionId, since: 0 });
            if (sent) return;
            // The socket is not open, which is exactly when a reader most wants
            // the history back. `GET /api/sessions/:id/events` answers the same
            // question over HTTP, so there is a way through rather than only a
            // message that cannot be sent.
            void rest.events(sessionId, 0, 0)
                .then((result) => {
                    store.getState().mergeTranscript(sessionId, result.events, result.latest);
                })
                .catch((error: unknown) => {
                    const detail = error instanceof ApiError
                        ? error.message
                        : error instanceof Error ? error.message : String(error);
                    store.getState().setNotice('error', 'transcript_unavailable', detail);
                });
        },

        reloadHistory(sessionId) {
            return requestHistory(sessionId);
        },

        ping() {
            const at = Date.now();
            const sent = socket.send({ type: 'ping' });
            if (sent) {
                pendingPing = at;
                store.getState().beginPing(at);
            }
            return sent;
        },

        refreshSessions() {
            socket.send({ type: 'list_sessions' });
        },

        sendInput(sessionId, parts, operation = 'message', options) {
            const requestId = newRequestId();
            store.getState().beginInput(sessionId, requestId, parts, operation);
            const sent = socket.send({
                type: 'input',
                session: sessionId,
                request_id: requestId,
                operation,
                ...(operation === 'continue' ? {} : { content: [...parts] }),
                ...(options ? { options } : {}),
            });
            if (!sent) {
                store.getState().failInput(
                    sessionId, requestId, 'the panel socket is not connected',
                );
            }
            return sent;
        },

        sendSignal(sessionId, operation, runId) {
            return socket.send({
                type: 'signal',
                session: sessionId,
                operation,
                ...(runId ? { run_id: runId } : {}),
            });
        },

        sendConfirmation(sessionId, confirmationId, decision, reason) {
            return socket.send({
                type: 'confirmation',
                session: sessionId,
                confirmation_id: confirmationId,
                decision,
                ...(reason ? { reason } : {}),
            });
        },

        refreshLogs(sessionId, limit) {
            return socket.send({
                type: 'logs',
                session: sessionId,
                ...(limit === undefined ? {} : { limit }),
            });
        },

        async loadSnapshot(sessionId) {
            store.getState().beginSnapshot(sessionId);
            try {
                const view = await rest.snapshot(sessionId);
                store.getState().finishSnapshot(sessionId, view, null);
            } catch (error) {
                const detail = error instanceof ApiError
                    ? error.message
                    : error instanceof Error ? error.message : String(error);
                store.getState().finishSnapshot(sessionId, null, detail);
            }
        },

        async workerAction(sessionId, action, spec) {
            try {
                const result = await rest.worker(sessionId, action, spec);
                if (!result.ok) {
                    store.getState().setNotice(
                        'warn', action, result.error || `the hub refused to ${action}`,
                    );
                }
            } catch (error) {
                reportApiError(error, `${action} failed`);
            }
        },

        async createSession(sessionId, spec) {
            try {
                const created = await rest.createSession(sessionId, spec);
                store.getState().upsertSession(created.session);
                return true;
            } catch (error) {
                reportApiError(error, `could not create "${sessionId}"`);
                return false;
            }
        },

        async deleteSession(sessionId) {
            try {
                await rest.deleteSession(sessionId);
                return true;
            } catch (error) {
                reportApiError(error, `could not delete "${sessionId}"`);
                return false;
            }
        },

        async setToken(value) {
            tokens.set(value);
            store.getState().setAuthRequired(false);
            try {
                store.getState().setHub(await rest.meta());
                store.getState().upsertSessions((await rest.sessions()).sessions);
            } catch (error) {
                reportApiError(error, 'that token was not accepted');
                return;
            }
            socket.reconnectNow();
        },
    };
}
