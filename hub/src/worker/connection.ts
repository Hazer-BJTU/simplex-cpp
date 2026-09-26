/**
 * @file the worker-facing event connection.
 *
 * Route: `GET /agent/<session_id>/events?token=<session token>`.
 *
 * Per core/docs/worker-protocol.md the worker is always the WebSocket client,
 * one process owns one session, and the event connection is persistent and
 * reconnecting. This module implements the hub's half of that:
 *
 *   - associate the upgrade with a deployment-authorized session (token),
 *   - validate envelopes and track `(worker_id, sequence)`,
 *   - keep the identity three-valued (unknown/live/stale) for the confirmation
 *     adapter, which must never judge a prompt against a stale identity,
 *   - send `status` immediately after the upgrade, because `ready` is emitted
 *     once per worker lifetime and may therefore never arrive on a reconnect.
 *
 * It deliberately does not reject a second event connection for one session.
 * The superseded connection is closed instead: a worker reconnecting after an
 * unobserved peer death is the common case, the worker retries a rejected
 * upgrade forever, and two live workers on one session are already prevented by
 * the worker's own session lock.
 */
import { WebSocketServer } from 'ws';
import type { RawData, WebSocket } from 'ws';
import type { Duplex } from 'node:stream';
import type { IncomingMessage } from 'node:http';
import { presentedToken, safeEqual } from '../http/auth.ts';
import { buildSignal } from '../protocol/messages.ts';
import { parseEventEnvelope } from '../protocol/events.ts';
import type { ParsedEnvelope, UnsignedInteger } from '../protocol/events.ts';
import { isValidSessionId } from '../state/session-id.ts';
import type { Session, SessionRegistry } from '../state/registry.ts';
import type { HubConfig } from '../config.ts';
import type { Logger } from '../log.ts';
import type { UpgradeContext, UpgradeHandler } from '../http/server.ts';

/** Upgrade path pattern for the persistent event connection. */
const EVENTS_ROUTE = /^\/agent\/([^/]+)\/events$/;

/** Protocol errors tolerated on one connection before it is closed. */
const MAX_PROTOCOL_ERRORS = 20;

/** Outbound backlog above which sends are refused rather than queued. */
const MAX_BUFFERED_BYTES = 4 * 1024 * 1024;

/** Close code used when a newer connection supersedes this one. */
export const CLOSE_SUPERSEDED = 4001;

/** Longest reason string a WebSocket close frame can carry. */
const MAX_CLOSE_REASON_BYTES = 123;

/** The outcome of trying to write to a worker socket. */
export type SendResult = { ok: true } | { ok: false; error: string };

/** One recorded protocol error. */
export interface ProtocolErrorRecord {
    at: string;
    message: string;
    fatal: boolean;
}

/**
 * A parsed envelope after the hub has annotated it.
 *
 * The hub adds its own sequence number, a receive timestamp, the validation
 * issues it found, and a note about the connection; the panel renders all four.
 */
export interface ForwardedEnvelope extends ParsedEnvelope {
    hub_sequence?: number;
    received_at?: string;
    issues?: string[];
    connection?: { opened_at: string; protocol_errors: number };
    /**
     * Any further field the worker sent. The registry stores envelopes by what
     * they are rather than by a fixed shape, and `raw` already keeps the
     * original document, so this is where an extension field lives.
     */
    [field: string]: unknown;
}

/** What `describe()` reports about one connection. */
export interface ConnectionDescription {
    opened_at: string;
    closed_at: string | null;
    open: boolean;
    close_reason: string | null;
    last_event_at: string | null;
    last_sequence: number | string | null;
    sent: number;
    protocol_errors: number;
    worker_id: string | null;
}

/**
 * Shorten a reason to what a WebSocket close frame can carry.
 */
export function truncateReason(reason: string): string {
    const bytes = Buffer.from(reason, 'utf8');
    return bytes.length <= MAX_CLOSE_REASON_BYTES
        ? reason
        : `${bytes.subarray(0, MAX_CLOSE_REASON_BYTES - 3).toString('utf8')}...`;
}

/** Write a plain HTTP rejection on a socket that never became a WebSocket. */
function rejectUpgrade(socket: Duplex, status: number, text: string): void {
    socket.write(`HTTP/1.1 ${status} ${text}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
    socket.destroy();
}

/** Everything `new WorkerConnection` needs. */
export interface WorkerConnectionOptions {
    ws: WebSocket;
    session: Session;
    config: HubConfig;
    log: Logger;
    onEvent?: ((envelope: ForwardedEnvelope, connection: WorkerConnection) => void) | undefined;
    onClosed?: ((connection: WorkerConnection) => void) | undefined;
}

/**
 * One live event connection to a worker process.
 *
 * The connection is the only way the hub talks to a worker, so it is also the
 * sender used by the panel and by the supervisor's graceful stop.
 */
export class WorkerConnection {
    readonly ws: WebSocket;
    readonly session: Session;
    readonly config: HubConfig;
    readonly log: Logger;
    onEvent: ((envelope: ForwardedEnvelope, connection: WorkerConnection) => void) | undefined;
    onClosed: ((connection: WorkerConnection) => void) | undefined;
    readonly openedAt: string;
    closedAt: string | null;
    closeReason: string | null;
    lastSequence: bigint | null;
    sequenceDisplay: number | string | null;
    lastEventAt: string | null;
    lastError: string | null;
    sent: number;
    readonly protocolErrors: ProtocolErrorRecord[];
    alive: boolean;
    pingTimer: NodeJS.Timeout | null;

    constructor({ ws, session, config, log, onEvent, onClosed }: WorkerConnectionOptions) {
        this.ws = ws;
        this.session = session;
        this.config = config;
        this.log = log;
        this.onEvent = onEvent;
        this.onClosed = onClosed;
        this.openedAt = new Date().toISOString();
        this.closedAt = null;
        this.closeReason = null;
        this.lastSequence = null;
        this.sequenceDisplay = null;
        this.lastEventAt = null;
        this.lastError = null;
        this.sent = 0;
        this.protocolErrors = [];
        this.alive = true;
        this.pingTimer = null;

        ws.on('message', (data: RawData, isBinary: boolean) => this.onMessage(data, isBinary));
        ws.on('close', (code: number, reason: Buffer) => this.onClose(code, reason));
        ws.on('error', (error: Error) => this.onError(error));
        ws.on('pong', () => { this.alive = true; });
        this.startPing();
        // Protocol requirement: a reconnect must not wait for `ready`.
        this.sendSignal(buildSignal({ operation: 'status' }));
    }

    /** True while the socket can still carry messages. */
    get isOpen(): boolean {
        return this.ws.readyState === this.ws.OPEN;
    }

    /** Serializable description for the panel. */
    describe(): ConnectionDescription {
        return {
            opened_at: this.openedAt,
            closed_at: this.closedAt,
            open: this.isOpen,
            close_reason: this.closeReason,
            last_event_at: this.lastEventAt,
            last_sequence: this.sequenceDisplay,
            sent: this.sent,
            protocol_errors: this.protocolErrors.length,
            worker_id: this.session.identity.workerId,
        };
    }

    /**
     * Send one JSON message.
     *
     * A successful send means the message entered this socket's buffer. The
     * worker protocol has no delivery acknowledgement, so the panel must still
     * treat the outcome as unknown until admission is observed.
     */
    send(message: unknown): SendResult {
        if (!this.isOpen) return { ok: false, error: 'worker is not connected' };
        if (this.ws.bufferedAmount > MAX_BUFFERED_BYTES) {
            return { ok: false, error: 'worker connection is congested; message not sent' };
        }
        let text: string;
        try {
            text = JSON.stringify(message);
        } catch (cause) {
            const message_ = cause instanceof Error ? cause.message : String(cause);
            return { ok: false, error: `message is not serializable: ${message_}` };
        }
        this.sent += 1;
        this.ws.send(text, (error?: Error) => {
            if (error) {
                this.lastError = error.message;
                this.log.warn(`send to ${this.session.id} failed: ${error.message}`);
            }
        });
        return { ok: true };
    }

    /**
     * Send a payload envelope.
     *
     * Typed as `unknown` because it is also reached through the registry's
     * `AttachedConnection` slice, where the concrete envelope type is not in
     * scope; `protocol/messages.ts` is what validates the shape before it gets
     * here.
     */
    sendPayload(payload: unknown): SendResult {
        return this.send(payload);
    }

    /** Send a signal envelope; see `sendPayload` for why this is `unknown`. */
    sendSignal(signal: unknown): SendResult {
        return this.send(signal);
    }

    /** Ask the worker to shut down (no acknowledgement exists). */
    requestShutdown(): SendResult {
        return this.sendSignal(buildSignal({ operation: 'shutdown' }));
    }

    /** Close the socket, starting a normal close handshake. */
    close(code = 1000, reason = 'hub closing connection'): void {
        if (this.ws.readyState === this.ws.CLOSED) return;
        this.closeReason ??= reason;
        try {
            this.ws.close(code, truncateReason(reason));
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            this.log.debug(`close failed for ${this.session.id}: ${message}`);
            this.ws.terminate();
        }
    }

    /** Drop the socket without a close handshake. */
    terminate(reason = 'terminated'): void {
        this.closeReason ??= reason;
        this.ws.terminate();
    }

    /** Keep the connection honest: a half-open socket must not look alive. */
    startPing(): void {
        const interval = this.config.limits.pingIntervalMs;
        if (!interval) return;
        this.pingTimer = setInterval(() => {
            if (!this.isOpen) return;
            if (!this.alive) {
                this.log.warn(`worker ${this.session.id} missed a ping; terminating`);
                this.terminate('ping timeout');
                return;
            }
            this.alive = false;
            try {
                this.ws.ping();
            } catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                this.log.debug(`ping failed for ${this.session.id}: ${message}`);
            }
        }, interval);
        this.pingTimer.unref?.();
    }

    /** Handle one inbound text message. */
    onMessage(data: RawData, isBinary: boolean): void {
        if (isBinary) {
            this.noteProtocolError('binary worker message');
            return;
        }
        const text = Buffer.isBuffer(data) ? data.toString('utf8') : String(data);
        const parsed = parseEventEnvelope(text);
        if (!parsed.ok) {
            this.noteProtocolError(parsed.error);
            return;
        }
        const { envelope, issues, sequence } = parsed;
        if (envelope.session_id !== this.session.id) {
            // The route already bound this connection to one session; a worker
            // claiming another one must not be trusted with either.
            this.noteProtocolError(
                `event session_id "${envelope.session_id}" does not match route session`);
            return;
        }
        for (const issue of issues) this.noteProtocolError(issue, { fatal: false });

        const { incarnation } = this.session.noteIdentity(envelope.worker_id);
        if (incarnation) {
            this.log.warn(`session ${this.session.id} is now served by worker ${envelope.worker_id}`);
        }
        this.trackSequence(sequence);

        const forwarded: ForwardedEnvelope = {
            ...envelope,
            received_at: new Date().toISOString(),
            hub_sequence: this.session.stats.events + 1,
            issues,
            connection: { opened_at: this.openedAt, protocol_errors: this.protocolErrors.length },
        };
        this.lastEventAt = forwarded.received_at as string;
        this.session.noteEnvelope(forwarded);
        if (!forwarded.known) {
            this.log.debug(`session ${this.session.id}: unknown event "${forwarded.event}"`);
        }
        this.emit(forwarded);
    }

    /**
     * Hand a validated envelope to the observer, containing its failure.
     *
     * This runs inside the socket's `message` listener, so a throwing observer
     * would surface as an uncaught exception rather than a rejected promise.
     * The envelope is already recorded by the time this is called; losing the
     * observer's reaction is strictly better than losing the hub.
     */
    emit(envelope: ForwardedEnvelope): void {
        try {
            this.onEvent?.(envelope, this);
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            this.log.error(`session ${this.session.id}: event observer failed: ${message}`, error);
        }
    }

    /** Update gap/duplicate counters from an event's sequence number. */
    trackSequence(sequence: UnsignedInteger): void {
        if (!sequence || sequence.value === null) return;
        this.sequenceDisplay = sequence.display;
        const value = sequence.value;
        if (this.lastSequence !== null) {
            if (value <= this.lastSequence) {
                this.session.stats.duplicates += 1;
                this.log.debug(`session ${this.session.id}: duplicate sequence ${value}`);
            } else if (value > this.lastSequence + 1n) {
                const missing = Number(value - this.lastSequence - 1n);
                this.session.stats.gaps += missing;
                this.log.warn(
                    `session ${this.session.id}: ${missing} event(s) missing between `
                    + `${this.lastSequence} and ${value}`);
            }
        }
        if (this.lastSequence === null || value > this.lastSequence) this.lastSequence = value;
    }

    /** Record a protocol error; repeated failures end the connection. */
    noteProtocolError(message: string, { fatal = true }: { fatal?: boolean } = {}): void {
        this.session.stats.protocolErrors += 1;
        this.protocolErrors.push({ at: new Date().toISOString(), message, fatal });
        this.log.warn(`session ${this.session.id}: protocol error: ${message}`);
        if (fatal && this.protocolErrors.length >= MAX_PROTOCOL_ERRORS) {
            this.close(1008, 'too many protocol errors');
        }
    }

    /** Socket error: logged, the close handler does the bookkeeping. */
    onError(error: Error): void {
        this.lastError = error.message;
        this.log.debug(`session ${this.session.id}: socket error: ${error.message}`);
    }

    /** Socket closed: detach from the session and tell the hub. */
    onClose(code: number, reasonBuffer: Buffer): void {
        if (this.pingTimer) clearInterval(this.pingTimer);
        this.pingTimer = null;
        this.closedAt = new Date().toISOString();
        const reason = reasonBuffer?.length ? reasonBuffer.toString('utf8') : '';
        this.closeReason ??= reason || `closed with code ${code}`;
        this.session.detach(this);
        this.log.info(`session ${this.session.id}: event connection closed (${code} ${this.closeReason})`);
        this.onClosed?.(this);
    }
}

/** Everything `createWorkerEventRoute` needs. */
export interface WorkerEventRouteOptions {
    registry: SessionRegistry;
    config: HubConfig;
    log: Logger;
    onEvent?: (envelope: ForwardedEnvelope, connection: WorkerConnection) => void;
    onConnectionChange?: (session: Session, connection: WorkerConnection | null) => void;
}

/** The worker event route, as a WebSocket upgrade handler. */
export interface WorkerEventRoute extends UpgradeHandler {
    close(): void;
}

/**
 * Build the worker-facing upgrade route for the hub's HTTP server.
 */
export function createWorkerEventRoute({
    registry, config, log, onEvent, onConnectionChange,
}: WorkerEventRouteOptions): WorkerEventRoute {
    const wss = new WebSocketServer({
        noServer: true,
        maxPayload: config.limits.maxMessageBytes,
        perMessageDeflate: false,
    });

    function accept(ws: WebSocket, session: Session): void {
        const connection = new WorkerConnection({
            ws,
            session,
            config,
            log: log.child(`worker:${session.id}`),
            onEvent,
            onClosed: (closed) => {
                // A superseded connection closes *after* its replacement is
                // already attached, so only the connection the session still
                // holds may report a disconnect. Without this the panel is told
                // `connected: false` while a live worker is attached.
                if (session.connection === closed) onConnectionChange?.(session, null);
            },
        });
        const { previous, replaced } = session.attach(connection);
        if (replaced && previous) {
            log.warn(`session ${session.id}: replacing an existing event connection`);
            (previous as WorkerConnection).close(
                CLOSE_SUPERSEDED, 'superseded by a newer worker connection');
        }
        log.info(`session ${session.id}: event connection open`);
        onConnectionChange?.(session, connection);
    }

    return {
        match(req: IncomingMessage, url: URL): Record<string, unknown> | null {
            if (req.method !== 'GET') return null;
            const matched = EVENTS_ROUTE.exec(url.pathname);
            if (!matched) return null;
            try {
                return { session: decodeURIComponent(matched[1] as string) };
            } catch {
                return null;
            }
        },

        handle({ req, socket, head, url, params }: UpgradeContext): void {
            const sessionId = params.session;
            if (!isValidSessionId(sessionId)) {
                log.warn(`rejected event upgrade: invalid session id "${String(sessionId)}"`);
                rejectUpgrade(socket, 404, 'Not Found');
                return;
            }
            const session = registry.get(sessionId);
            if (!session) {
                log.warn(`rejected event upgrade: unknown session "${sessionId}"`);
                rejectUpgrade(socket, 404, 'Not Found');
                return;
            }
            if (!safeEqual(presentedToken(url), session.token)) {
                log.warn(`rejected event upgrade for ${sessionId}: bad token`);
                rejectUpgrade(socket, 401, 'Unauthorized');
                return;
            }
            wss.handleUpgrade(req, socket, head, (ws) => accept(ws, session));
        },

        close(): void {
            for (const client of wss.clients) {
                try {
                    client.close(1001, 'hub shutting down');
                } catch {
                    client.terminate();
                }
            }
            wss.close();
        },
    };
}
