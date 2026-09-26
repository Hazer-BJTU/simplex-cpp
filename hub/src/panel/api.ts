/**
 * @file the hub's own client protocol: JSON API plus a panel WebSocket.
 *
 * This is *not* the worker protocol. A browser never speaks
 * core/docs/worker-protocol.md directly: it must not be able to choose
 * `confirmation.mode: approve` on the worker-facing channel, and the worker
 * socket is a deployment-trusted endpoint. The hub therefore owns the trust
 * boundary and exposes its own versioned protocol, documented in
 * hub/docs/hub-protocol.md.
 *
 * Every message carries `v: 1`. Unknown message types are ignored and unknown
 * fields are preserved, so a newer panel can talk to an older hub and vice
 * versa. Worker events are forwarded verbatim inside `envelope` — the panel is
 * the only place that renders them.
 *
 * The message shapes are not declared here: they come from
 * `shared/protocol.ts`, the same module the panel imports, so what this file
 * sends and what the browser expects cannot drift apart without a type error.
 */
import { existsSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { WebSocketServer } from 'ws';
import type { RawData, WebSocket } from 'ws';
import { authorizePanel, presentedToken, safeEqual } from '../http/auth.ts';
import { readJsonBody, sendError, sendJson } from '../http/server.ts';
import type { UpgradeContext, UpgradeHandler } from '../http/server.ts';
import type { RouteHandler } from '../http/router.ts';
import { persistenceRoot } from '../launch/config-render.ts';
import { normalizeSpec } from '../launch/spec.ts';
import { buildPayload, buildSignal, newRequestId } from '../protocol/messages.ts';
import type { PayloadEnvelope, SignalEnvelope } from '../protocol/messages.ts';
import { isValidSessionId } from '../state/session-id.ts';
import type { Session, SessionRegistry } from '../state/registry.ts';
import type { HubState, PersistableSession } from '../state/persist.ts';
import type { TranscriptStore } from '../state/transcript.ts';
import type { WorkerSupervisor } from '../launch/supervisor.ts';
import type { WorkerConnection, ForwardedEnvelope } from '../worker/connection.ts';
import type { PendingConfirmation } from '../worker/confirmation.ts';
import { checkEnvelope } from '../../shared/guards.ts';
import { PANEL_VERSION, SESSIONLESS_MESSAGE_TYPES } from '../../shared/protocol.ts';
import type {
    Capability,
    ConfirmationOutcome,
    HubMessage,
    HubMetadata,
    SessionSpec,
    WorkerEnvelope,
} from '../../shared/protocol.ts';
import type { HubConfig } from '../config.ts';
import type { Logger } from '../log.ts';

// The version lives in `shared/protocol.ts` so the hub and the panel cannot
// disagree about it. Re-exported because this module is where a reader expects
// to find it, and because that is the name every message is stamped with.
export { PANEL_VERSION };

/** Largest on-disk artifact the snapshot viewer will read. */
const SNAPSHOT_MAX_BYTES = 8 * 1024 * 1024;

/** Log lines returned by one explicit logs request. */
const LOG_TAIL_DEFAULT = 200;
const LOG_TAIL_MAX = 2000;

/** The read-only view of a session's persisted files. */
export interface SnapshotView {
    session_id: string;
    state: unknown;
    readable: string | null;
    files: Record<string, string>;
    state_error?: string;
}

/** One connected panel client. */
export interface PanelClient {
    ws: WebSocket;
    subscriptions: Set<string>;
    openedAt: string;
    remote: string | undefined;
}

/** A refusal from an input or signal attempt. */
type SendFailure = { ok: false; error: string };

/** The observer hooks the rest of the hub reports through. */
export interface PanelHooks {
    onEvent(envelope: ForwardedEnvelope, connection: WorkerConnection): void;
    onPrompt(prompt: PendingConfirmation): void;
    onPromptSettled(prompt: PendingConfirmation, outcome: ConfirmationOutcome): void;
    onProcessChange(session: Session, record: unknown): void;
    onConnectionChange(session: Session, connection: WorkerConnection | null): void;
}

/** The panel API facade. */
export interface PanelApi {
    version: number;
    hooks: PanelHooks;
    upgrade: UpgradeHandler;
    routes: Record<string, RouteHandler>;
    broadcast(message: HubMessage): void;
    broadcastSession(session: Session): void;
    /** Number of connected panel clients, for diagnostics and tests. */
    clientCount(): number;
    close(): void;
}

/** Everything `createPanelApi` needs. */
export interface PanelApiOptions {
    config: HubConfig;
    log: Logger;
    registry: SessionRegistry;
    supervisor: WorkerSupervisor;
    transcripts: TranscriptStore;
    state: HubState;
    /** Metadata for `welcome` and `/api/meta`. */
    meta: () => HubMetadata;
    /** Persistence hook, called for a debounced save. */
    onSessionsChanged?: ((sessions: PersistableSession[]) => void) | undefined;
}

/**
 * Build the panel API: REST routes, the panel WebSocket, and the observer hooks
 * the rest of the hub reports through.
 */
export function createPanelApi({
    config, log, registry, supervisor, transcripts, state, meta, onSessionsChanged,
}: PanelApiOptions): PanelApi {
    const clients = new Set<PanelClient>();
    const wss = new WebSocketServer({
        noServer: true,
        maxPayload: 4 * 1024 * 1024,
        perMessageDeflate: false,
    });

    /** Send one versioned message to a panel client. */
    function send(client: PanelClient, message: HubMessage): void {
        if (client.ws.readyState !== client.ws.OPEN) return;
        client.ws.send(JSON.stringify({ v: PANEL_VERSION, ...message }));
    }

    /** Send to every client subscribed to a session. */
    function broadcastToSession(sessionId: string, message: HubMessage): void {
        for (const client of clients) {
            if (client.subscriptions.has(sessionId)) send(client, message);
        }
    }

    /** Send to every connected panel client. */
    function broadcast(message: HubMessage): void {
        for (const client of clients) send(client, message);
    }

    /** Broadcast the current description of one session. */
    function broadcastSession(session: Session): void {
        broadcast({ type: 'session', session: session.describe() });
    }

    /**
     * Persist the session list after a change.
     *
     * Process changes are written immediately: adoption after a hub restart
     * depends on the recorded pid, and a worker that starts and outlives a
     * crash must not be lost to a pending debounce.
     */
    function persist({ immediate = false }: { immediate?: boolean } = {}): void {
        const sessions = registry.list();
        if (immediate) state.flush(sessions);
        else onSessionsChanged?.(sessions);
    }

    // ---------------------------------------------------------------------
    // Observer hooks
    // ---------------------------------------------------------------------

    const hooks: PanelHooks = {
        onEvent: (envelope, connection) => {
            const session = connection.session;
            if (envelope.event === 'history') {
                // Preserve its sequence position for replay cursors without
                // retaining or logging the potentially large display body.
                const data = envelope.data as Record<string, unknown> | null;
                const marker = {
                    ...envelope,
                    data: {
                        request_id: data?.request_id,
                        revision: data?.revision,
                        start: data?.start,
                        step: data?.step,
                        next: data?.next,
                        next_step: data?.next_step,
                        total: data?.total,
                    },
                    raw: null,
                    transient_history: true,
                    bytes: 0,
                };
                marker.bytes = Buffer.byteLength(JSON.stringify(marker), 'utf8');
                transcripts.get(session.id).append(marker);
                envelope.hub_sequence = marker.hub_sequence ?? 0;
            } else {
                transcripts.get(session.id).append(envelope);
            }
            if (envelope.event === 'input_admitted') {
                if (session.noteRequestAdmitted(envelope.request_id)) {
                    broadcastToSession(session.id, {
                        type: 'request',
                        session: session.id,
                        request: session.requests.get(envelope.request_id) as never,
                    });
                }
            } else if (envelope.event === 'input_rejected') {
                const data = envelope.data as { request_id?: unknown; message?: unknown } | null;
                const requestId = data?.request_id;
                if (typeof requestId === 'string'
                    && session.noteRequestRejected(requestId,
                        typeof data?.message === 'string' ? data.message : null)) {
                    broadcastToSession(session.id, {
                        type: 'request',
                        session: session.id,
                        request: session.requests.get(requestId) as never,
                    });
                }
            }
            broadcastToSession(session.id, {
                type: 'event',
                session: session.id,
                hub_seq: envelope.hub_sequence ?? 0,
                envelope,
            });
        },

        /**
         * A confirmation goes to *every* client, not only the subscribers of its
         * session.
         *
         * This was subscription-scoped, and that made an approval impossible to
         * answer whenever the operator happened to be looking at another
         * session: the prompt was broadcast to a set the client was not in, so
         * the only trace of it was a count badge, and the worker's own deadline
         * denied it. An approval is the one thing that must not be missed, and
         * every client on this socket already shares the panel token, so
         * widening the audience grants no authority that was not already there.
         */
        onPrompt: (prompt) => {
            broadcast({
                type: 'confirmation',
                session: prompt.session.id,
                open: true,
                confirmation: prompt.describe(),
            });
            broadcastSession(prompt.session);
        },

        onPromptSettled: (prompt, outcome) => {
            broadcast({
                type: 'confirmation',
                session: prompt.session.id,
                open: false,
                outcome,
                confirmation: prompt.describe(),
            });
            broadcastSession(prompt.session);
        },

        onProcessChange: (session) => {
            broadcastToSession(session.id, {
                type: 'process',
                session: session.id,
                process: session.process?.describe() ?? null,
            });
            broadcastSession(session);
            persist({ immediate: true });
        },

        onConnectionChange: (session, connection) => {
            broadcastToSession(session.id, {
                type: 'connection',
                session: session.id,
                connected: connection !== null,
                identity: {
                    state: session.identity.state,
                    worker_id: session.identity.workerId,
                    since: session.identity.since,
                },
            });
            broadcastSession(session);
        },
    };

    // ---------------------------------------------------------------------
    // Shared helpers
    // ---------------------------------------------------------------------

    /** Load a session or send a 404. */
    function requireSession(res: Parameters<typeof sendError>[0], sessionId: string): Session | null {
        if (!isValidSessionId(sessionId)) {
            sendError(res, 400, 'invalid_session', 'session id must be 1-128 [A-Za-z0-9_-]');
            return null;
        }
        const session = registry.get(sessionId);
        if (!session) {
            sendError(res, 404, 'unknown_session', `unknown session "${sessionId}"`);
            return null;
        }
        return session;
    }

    /**
     * Check a session spec before the session is stored.
     *
     * A spec that cannot produce a worker is refused here, while the operator is
     * still looking at the form, instead of being persisted and only surfacing
     * later as a start failure. What gets stored is deliberately unchanged: the
     * defaults are applied at start, which is also when the resolved spec is
     * echoed back to the panel.
     *
     * @returns an error message, or null when the spec is usable.
     */
    function checkSpec(rawSpec: unknown): string | null {
        if (rawSpec === undefined || rawSpec === null) return null;
        if (typeof rawSpec !== 'object' || Array.isArray(rawSpec)) {
            return 'spec must be a JSON object';
        }
        try {
            normalizeSpec(config, rawSpec);
            return null;
        } catch (error) {
            return error instanceof Error ? error.message : String(error);
        }
    }

    /**
     * Re-type a replayed transcript for the wire.
     *
     * The transcript store measures envelopes — it needs `bytes` and writes
     * `hub_sequence` — so its element type is narrower than the envelope the
     * panel receives. They are the same objects; only the declared view differs.
     */
    function asEnvelopes(items: unknown[]): WorkerEnvelope[] {
        return items as WorkerEnvelope[];
    }

    /** Read the tail of a session's on-disk snapshot, read-only. */
    function readSnapshot(session: Session): SnapshotView {
        const directory = join(persistenceRoot(config), session.id);
        const statePath = join(directory, 'state.json');
        const readablePath = join(directory, 'readable.md');
        const result: SnapshotView = { session_id: session.id, state: null, readable: null, files: {} };
        if (existsSync(statePath) && statSync(statePath).size <= SNAPSHOT_MAX_BYTES) {
            result.files.state = statePath;
            try {
                result.state = JSON.parse(readFileSync(statePath, 'utf8'));
            } catch (error) {
                result.state_error = error instanceof Error ? error.message : String(error);
            }
        }
        if (existsSync(readablePath) && statSync(readablePath).size <= SNAPSHOT_MAX_BYTES) {
            result.files.readable = readablePath;
            result.readable = readFileSync(readablePath, 'utf8');
        }
        return result;
    }

    /** Build and send a payload on behalf of the panel. */
    function sendInput(
        session: Session,
        body: { request_id?: unknown; operation?: unknown; content?: unknown; options?: unknown },
    ): { ok: true; request_id: string } | SendFailure {
        const connection = session.connection;
        if (!connection?.isOpen) return { ok: false, error: 'the worker is not connected' };
        const requestId = typeof body.request_id === 'string' && body.request_id.length > 0
            ? body.request_id
            : newRequestId();
        let payload: PayloadEnvelope;
        try {
            payload = buildPayload({
                operation: (body.operation ?? 'message') as 'message' | 'continue',
                requestId,
                content: body.content,
                options: body.options,
            });
        } catch (error) {
            return { ok: false, error: error instanceof Error ? error.message : String(error) };
        }
        const entry = session.trackRequest(payload.data.request_id, payload.data.operation);
        const sent = connection.sendPayload(payload);
        if (!sent.ok) {
            session.requests.delete(payload.data.request_id);
            return { ok: false, error: sent.error ?? 'the payload was not sent' };
        }
        broadcastToSession(session.id, { type: 'request', session: session.id, request: entry });
        return { ok: true, request_id: payload.data.request_id };
    }

    /** Read-only history queries have their own panel command and no run admission. */
    function sendHistory(session: Session, message: {
        request_id?: string; start?: number; step?: number; limit?: number;
    }): { ok: true; request_id: string } | SendFailure {
        const connection = session.connection;
        if (!connection?.isOpen) return { ok: false, error: 'the worker is not connected' };
        try {
            const payload = buildPayload({
                operation: 'history', requestId: message.request_id ?? newRequestId(),
                ...(message.start === undefined ? {} : { start: message.start }),
                ...(message.step === undefined ? {} : { step: message.step }),
                ...(message.limit === undefined ? {} : { limit: message.limit }),
            });
            const sent = connection.sendPayload(payload);
            return sent.ok ? { ok: true, request_id: payload.data.request_id }
                : { ok: false, error: sent.error ?? 'the history query was not sent' };
        } catch (error) {
            return { ok: false, error: error instanceof Error ? error.message : String(error) };
        }
    }

    /** Build and send a signal on behalf of the panel. */
    function sendSignal(
        session: Session,
        body: { operation: string; run_id?: unknown },
    ): { ok: true } | SendFailure {
        const connection = session.connection;
        if (!connection?.isOpen) return { ok: false, error: 'the worker is not connected' };
        const runId = typeof body.run_id === 'string'
            ? body.run_id
            : (body.operation === 'cancel' ? session.lastRunId : undefined);
        let signal: SignalEnvelope;
        try {
            signal = buildSignal({
                operation: body.operation as 'status' | 'options' | 'cancel' | 'shutdown',
                runId,
            });
        } catch (error) {
            return { ok: false, error: error instanceof Error ? error.message : String(error) };
        }
        const sent = connection.sendSignal(signal);
        return sent.ok ? { ok: true } : { ok: false, error: sent.error ?? 'the signal was not sent' };
    }

    /** Apply a worker action requested by the panel. */
    async function workerAction(
        session: Session,
        action: string,
        spec: unknown,
    ): Promise<{ ok: boolean; error?: string | undefined; [field: string]: unknown }> {
        // Spread into a fresh literal: the supervisor's named result types are
        // what the panel receives verbatim, and this is the point where they
        // become an opaque `result` field on the wire.
        switch (action) {
            case 'start':
                return { ...(await supervisor.start(session, spec)) };
            case 'stop':
                return { ...(await supervisor.stop(session)) };
            case 'restart':
                return { ...(await supervisor.restart(session, spec)) };
            case 'force-kill':
                return { ...(await supervisor.forceKill(session)) };
            default:
                return { ok: false, error: `unknown worker action "${action}"` };
        }
    }

    // ---------------------------------------------------------------------
    // REST
    // ---------------------------------------------------------------------

    const routes: Record<string, RouteHandler> = {
        'GET /api/sessions': ({ res }) => {
            sendJson(res, 200, {
                sessions: registry.list().map((session) => session.describe()),
            });
        },

        'POST /api/sessions': async ({ req, res }) => {
            const parsed = await readJsonBody(req, config.limits.maxMessageBytes) as
                { session?: unknown; spec?: unknown };
            const id = parsed.session;
            if (!isValidSessionId(id)) {
                sendError(res, 400, 'invalid_session', 'session id must be 1-128 [A-Za-z0-9_-]');
                return;
            }
            if (registry.get(id)) {
                sendError(res, 409, 'session_exists', `session "${id}" already exists`);
                return;
            }
            const rawSpec = (parsed.spec ?? {}) as SessionSpec;
            const specError = checkSpec(rawSpec);
            if (specError) {
                sendError(res, 400, 'invalid_session', specError);
                return;
            }
            try {
                const session = registry.create(id, rawSpec);
                session.spec = rawSpec;
                persist();
                broadcastSession(session);
                sendJson(res, 201, { session: session.describe() });
            } catch (error) {
                sendError(res, 400, 'invalid_session',
                    error instanceof Error ? error.message : String(error));
            }
        },

        'GET /api/sessions/:id': ({ res, params }) => {
            const session = requireSession(res, params.id as string);
            if (!session) return;
            sendJson(res, 200, { session: session.describe() });
        },

        'DELETE /api/sessions/:id': ({ res, params }) => {
            const session = requireSession(res, params.id as string);
            if (!session) return;
            if (supervisor.isRunning(session) || session.connected) {
                sendError(res, 409, 'session_busy',
                    'stop the worker and disconnect it before deleting the session');
                return;
            }
            transcripts.remove(session.id);
            registry.remove(session.id);
            persist();
            broadcast({ type: 'session_removed', session: session.id });
            sendJson(res, 200, { removed: session.id });
        },

        'POST /api/sessions/:id/start': async ({ req, res, params }) => {
            const session = requireSession(res, params.id as string);
            if (!session) return;
            const parsed = await readJsonBody(req, config.limits.maxMessageBytes) as { spec?: unknown };
            const result = await supervisor.start(session, parsed.spec);
            persist();
            broadcastSession(session);
            sendJson(res, result.ok ? 200 : 409, result as unknown as Record<string, unknown>);
        },

        'POST /api/sessions/:id/stop': async ({ res, params }) => {
            const session = requireSession(res, params.id as string);
            if (!session) return;
            const result = await supervisor.stop(session);
            persist();
            broadcastSession(session);
            sendJson(res, 200, result);
        },

        'POST /api/sessions/:id/restart': async ({ req, res, params }) => {
            const session = requireSession(res, params.id as string);
            if (!session) return;
            const parsed = await readJsonBody(req, config.limits.maxMessageBytes) as { spec?: unknown };
            const result = await supervisor.restart(session, parsed.spec);
            persist();
            broadcastSession(session);
            sendJson(res, result.ok ? 200 : 409, result as unknown as Record<string, unknown>);
        },

        'POST /api/sessions/:id/force-kill': async ({ res, params }) => {
            const session = requireSession(res, params.id as string);
            if (!session) return;
            const result = await supervisor.forceKill(session);
            persist();
            broadcastSession(session);
            sendJson(res, 200, result as unknown as Record<string, unknown>);
        },

        'GET /api/sessions/:id/events': ({ res, url, params }) => {
            const session = requireSession(res, params.id as string);
            if (!session) return;
            const since = Number.parseInt(url.searchParams.get('since') ?? '0', 10) || 0;
            const limit = Number.parseInt(url.searchParams.get('limit') ?? '0', 10) || 0;
            sendJson(res, 200, {
                session: session.id,
                since,
                latest: transcripts.get(session.id).sequence,
                events: transcripts.get(session.id).since(since, limit),
            });
        },

        'GET /api/sessions/:id/logs': ({ res, url, params }) => {
            const session = requireSession(res, params.id as string);
            if (!session) return;
            const limit = Math.min(
                Number.parseInt(url.searchParams.get('limit') ?? String(LOG_TAIL_DEFAULT), 10)
                    || LOG_TAIL_DEFAULT,
                LOG_TAIL_MAX);
            sendJson(res, 200, {
                session: session.id,
                lines: supervisor.logs(session, { limit }),
                dropped: session.process?.logs?.dropped ?? 0,
                log_path: session.process?.logPath ?? null,
            });
        },

        'GET /api/sessions/:id/snapshot': ({ res, params }) => {
            const session = requireSession(res, params.id as string);
            if (!session) return;
            sendJson(res, 200, readSnapshot(session));
        },
    };

    // ---------------------------------------------------------------------
    // Panel WebSocket
    // ---------------------------------------------------------------------

    /** Handle one panel message. */
    async function handleMessage(client: PanelClient, raw: RawData): Promise<void> {
        // The envelope check is shared with the panel (`shared/guards.ts`): the
        // same code decides what a valid frame is at both ends, so a message the
        // hub refuses is one the panel never meant to send.
        const check = checkEnvelope(raw.toString());
        if (check.kind === 'rejected') {
            send(client, { type: 'error', error: check.code, message: check.detail });
            return;
        }
        if (check.kind === 'unknown_type') {
            // Forward compatibility: a newer panel may send a type this hub does
            // not know, and being ignored is the documented outcome.
            log.debug(`ignoring unknown panel message type "${check.type}"`);
            return;
        }
        const message = check.message;
        const type = message.type;
        const sessionId = 'session' in message ? message.session : undefined;
        const session = typeof sessionId === 'string' ? registry.get(sessionId) ?? null : null;

        const needsSession = !(SESSIONLESS_MESSAGE_TYPES as readonly string[]).includes(type);
        if (needsSession && !session) {
            send(client, {
                type: 'error',
                error: 'unknown_session',
                message: `unknown session "${String(sessionId)}"`,
                request: message,
            });
            return;
        }
        // The session is present for every type past this point; the branch
        // above returns for the types where it may be absent.
        const target = session as Session;

        switch (message.type) {
            case 'ping':
                send(client, { type: 'pong', at: new Date().toISOString() });
                return;
            case 'list_sessions':
                send(client, { type: 'sessions', sessions: registry.list().map((s) => s.describe()) });
                return;
            case 'subscribe': {
                client.subscriptions.add(target.id);
                send(client, {
                    type: 'subscribed',
                    session: target.describe(),
                    // The transcript measures envelopes rather than describing
                    // them, so its element type is the narrower one.
                    transcript: asEnvelopes(transcripts.get(target.id).since(Number(message.since) || 0)),
                    logs: supervisor.logs(target, { limit: LOG_TAIL_DEFAULT }),
                    latest: transcripts.get(target.id).sequence,
                    // Echoed so a client can tell "nothing new" apart from "your
                    // cursor predates a restart, and this hub's sequence started
                    // over". Without it the second case looks exactly like the
                    // first and the transcript just appears empty.
                    transcript_epoch: meta().transcript_epoch,
                });
                return;
            }
            case 'unsubscribe':
                client.subscriptions.delete(message.session);
                return;
            case 'create_session': {
                const created = message.session;
                if (!isValidSessionId(created)) {
                    send(client, {
                        type: 'error', error: 'invalid_session',
                        message: 'session id must be 1-128 [A-Za-z0-9_-]', request: message,
                    });
                    return;
                }
                if (registry.get(created)) {
                    send(client, {
                        type: 'error', error: 'session_exists',
                        message: `session "${created}" already exists`, request: message,
                    });
                    return;
                }
                const specError = checkSpec(message.spec);
                if (specError) {
                    send(client, {
                        type: 'error', error: 'invalid_session',
                        message: specError, request: message,
                    });
                    return;
                }
                const fresh = registry.create(created, message.spec ?? {});
                fresh.spec = message.spec ?? {};
                persist();
                broadcastSession(fresh);
                send(client, { type: 'created', session: fresh.describe() });
                return;
            }
            case 'delete_session': {
                if (supervisor.isRunning(target) || target.connected) {
                    send(client, {
                        type: 'error', error: 'session_busy',
                        message: 'stop the worker and disconnect it before deleting the session',
                        request: message,
                    });
                    return;
                }
                transcripts.remove(target.id);
                registry.remove(target.id);
                client.subscriptions.delete(target.id);
                persist();
                broadcast({ type: 'session_removed', session: target.id });
                return;
            }
            case 'worker': {
                const result = await workerAction(target, message.action, message.spec);
                persist();
                broadcastSession(target);
                if (result.ok) {
                    send(client, {
                        type: 'accepted',
                        action: message.action,
                        session: target.id,
                        result: result as never,
                    });
                } else {
                    send(client, {
                        type: 'error',
                        error: 'worker_action_failed',
                        message: result.error ?? '',
                        action: message.action,
                        session: target.id,
                        result: result as never,
                    });
                }
                return;
            }
            case 'input': {
                const result = sendInput(target, message);
                if (!result.ok) {
                    send(client, {
                        type: 'error', error: 'input_not_sent',
                        message: result.error, request: message,
                    });
                    return;
                }
                send(client, {
                    type: 'accepted', action: 'input', session: target.id,
                    request_id: result.request_id,
                });
                return;
            }
            case 'history': {
                const result = sendHistory(target, message);
                if (!result.ok) {
                    send(client, { type: 'error', error: 'input_not_sent',
                        message: result.error, request: message });
                    return;
                }
                send(client, { type: 'accepted', action: 'history', session: target.id,
                    request_id: result.request_id });
                return;
            }
            case 'signal': {
                const result = sendSignal(target, message);
                if (!result.ok) {
                    send(client, {
                        type: 'error', error: 'signal_not_sent',
                        message: result.error, request: message,
                    });
                    return;
                }
                send(client, {
                    type: 'accepted', action: 'signal',
                    operation: message.operation, session: target.id,
                });
                return;
            }
            case 'confirmation': {
                const prompt = target.prompts.get(message.confirmation_id);
                if (!prompt) {
                    send(client, {
                        type: 'error', error: 'unknown_confirmation',
                        message: 'that confirmation is no longer open', request: message,
                    });
                    return;
                }
                const result = prompt.decide(message.decision, message.reason ?? 'operator decision');
                if (result.ok) {
                    send(client, {
                        type: 'accepted',
                        action: 'confirmation',
                        session: target.id,
                        confirmation_id: message.confirmation_id,
                    });
                } else {
                    send(client, {
                        type: 'error',
                        error: 'confirmation_rejected',
                        message: result.error ?? '',
                        action: 'confirmation',
                        session: target.id,
                        confirmation_id: message.confirmation_id,
                    });
                }
                return;
            }
            case 'logs': {
                const limit = Math.min(Number(message.limit) || LOG_TAIL_DEFAULT, LOG_TAIL_MAX);
                send(client, {
                    type: 'logs',
                    session: target.id,
                    lines: supervisor.logs(target, { limit }),
                    dropped: target.process?.logs?.dropped ?? 0,
                });
                return;
            }
            case 'status_snapshot': {
                send(client, {
                    type: 'snapshot',
                    session: target.describe(),
                    transcript: asEnvelopes(transcripts.get(target.id).since(Number(message.since) || 0)),
                });
                return;
            }
            default:
                log.debug(`panel: ignoring unknown message type ${JSON.stringify(type)}`);
        }
    }

    function accept(ws: WebSocket, req: Parameters<UpgradeHandler['handle']>[0]['req']): void {
        const client: PanelClient = {
            ws,
            subscriptions: new Set(),
            openedAt: new Date().toISOString(),
            remote: req.socket.remoteAddress ?? undefined,
        };
        clients.add(client);
        ws.on('message', (data: RawData, isBinary: boolean) => {
            if (isBinary) {
                send(client, {
                    type: 'error', error: 'binary_not_supported',
                    message: 'panel messages must be text JSON',
                });
                return;
            }
            // A rejected handler must not escape: the hub treats an unhandled
            // rejection as fatal, and a message it cannot answer is still far
            // better than a hub that stops serving every other session.
            handleMessage(client, data).catch((error: Error) => {
                log.error(`panel message failed: ${error.message}`, error);
                send(client, {
                    type: 'error', error: 'internal_error',
                    message: 'the hub failed to process that message',
                });
            });
        });
        ws.on('close', () => clients.delete(client));
        ws.on('error', (error: Error) => log.debug(`panel socket error: ${error.message}`));
        send(client, {
            type: 'welcome',
            hub: meta(),
            sessions: registry.list().map((session) => session.describe()),
            subscriptions: [],
        });
    }

    const upgrade: UpgradeHandler = {
        match(req, url) {
            if (req.method !== 'GET' || url.pathname !== '/panel/ws') return null;
            return {};
        },
        handle({ req, socket, head, url }: UpgradeContext) {
            if (!authorizePanel(config, req, url).ok) {
                log.warn('rejected panel upgrade: bad token');
                socket.write('HTTP/1.1 401 Unauthorized\r\nConnection: close\r\n'
                    + 'Content-Length: 0\r\n\r\n');
                socket.destroy();
                return;
            }
            // Cross-site WebSocket hijacking: a browser sends Origin on an
            // upgrade, and a page the operator visits must not be able to drive
            // the hub just because it can reach loopback.
            //
            // A *missing* Origin is not the same as a wrong one, and it is
            // deliberately allowed: every browser sends one, so its absence
            // means a non-browser client — `wscat`, a script, another tool —
            // which must already hold the panel token and can reach the HTTP
            // API directly anyway. Rejecting it would break the "or any operator
            // tool" half of this protocol to defend against a caller that has
            // already been let in. A sandboxed frame is the case worth naming,
            // and it sends the literal `null`, which fails the comparison below.
            const origin = req.headers.origin;
            if (typeof origin === 'string' && origin.length > 0) {
                let originHost: string | null = null;
                try {
                    originHost = new URL(origin).host;
                } catch {
                    originHost = null;
                }
                if (originHost !== req.headers.host) {
                    log.warn(`rejected panel upgrade from origin ${origin}`);
                    socket.write('HTTP/1.1 403 Forbidden\r\nConnection: close\r\n'
                        + 'Content-Length: 0\r\n\r\n');
                    socket.destroy();
                    return;
                }
            }
            wss.handleUpgrade(req, socket, head, (ws) => accept(ws, req));
        },
    };

    return {
        version: PANEL_VERSION,
        hooks,
        upgrade,
        routes,
        broadcast,
        broadcastSession,
        clientCount: () => clients.size,
        close() {
            for (const client of clients) {
                try {
                    client.ws.close(1001, 'hub shutting down');
                } catch {
                    client.ws.terminate();
                }
            }
            clients.clear();
            wss.close();
        },
    };
}

/** Constant-time comparison re-exported for panel tests. */
export { safeEqual, presentedToken };

/** Capability names this module's behaviour implements. */
export type { Capability };
