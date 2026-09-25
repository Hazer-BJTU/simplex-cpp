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
import { presentedToken, safeEqual } from '../http/auth.js';
import { buildSignal } from '../protocol/messages.js';
import { parseEventEnvelope } from '../protocol/events.js';
import { isValidSessionId } from '../state/session-id.js';

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

function truncateReason(reason) {
    const bytes = Buffer.from(reason, 'utf8');
    return bytes.length <= MAX_CLOSE_REASON_BYTES
        ? reason
        : `${bytes.subarray(0, MAX_CLOSE_REASON_BYTES - 3).toString('utf8')}...`;
}

/** Write a plain HTTP rejection on a socket that never became a WebSocket. */
function rejectUpgrade(socket, status, text) {
    socket.write(`HTTP/1.1 ${status} ${text}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
    socket.destroy();
}

/**
 * One live event connection to a worker process.
 *
 * The connection is the only way the hub talks to a worker, so it is also the
 * sender used by the panel and by the supervisor's graceful stop.
 */
export class WorkerConnection {
    constructor({ ws, session, config, log, onEvent, onClosed }) {
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

        ws.on('message', (data, isBinary) => this.onMessage(data, isBinary));
        ws.on('close', (code, reason) => this.onClose(code, reason));
        ws.on('error', (error) => this.onError(error));
        ws.on('pong', () => { this.alive = true; });
        this.startPing();
        // Protocol requirement: a reconnect must not wait for `ready`.
        this.sendSignal(buildSignal({ operation: 'status' }));
    }

    /** True while the socket can still carry messages. */
    get isOpen() {
        return this.ws.readyState === this.ws.OPEN;
    }

    /** Serializable description for the panel. */
    describe() {
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
     *
     * @returns {{ok: true}|{ok: false, error: string}}
     */
    send(message) {
        if (!this.isOpen) return { ok: false, error: 'worker is not connected' };
        if (this.ws.bufferedAmount > MAX_BUFFERED_BYTES) {
            return { ok: false, error: 'worker connection is congested; message not sent' };
        }
        let text;
        try {
            text = JSON.stringify(message);
        } catch (cause) {
            return { ok: false, error: `message is not serializable: ${cause.message}` };
        }
        this.sent += 1;
        this.ws.send(text, (error) => {
            if (error) {
                this.lastError = error.message;
                this.log.warn(`send to ${this.session.id} failed: ${error.message}`);
            }
        });
        return { ok: true };
    }

    /** Send a validated payload envelope. */
    sendPayload(payload) {
        return this.send(payload);
    }

    /** Send a signal envelope. */
    sendSignal(signal) {
        return this.send(signal);
    }

    /** Ask the worker to shut down (no acknowledgement exists). */
    requestShutdown() {
        return this.sendSignal(buildSignal({ operation: 'shutdown' }));
    }

    /** Close the socket, starting a normal close handshake. */
    close(code = 1000, reason = 'hub closing connection') {
        if (this.ws.readyState === this.ws.CLOSED) return;
        this.closeReason ??= reason;
        try {
            this.ws.close(code, truncateReason(reason));
        } catch (error) {
            this.log.debug(`close failed for ${this.session.id}: ${error.message}`);
            this.ws.terminate();
        }
    }

    /** Drop the socket without a close handshake. */
    terminate(reason = 'terminated') {
        this.closeReason ??= reason;
        this.ws.terminate();
    }

    /** Keep the connection honest: a half-open socket must not look alive. */
    startPing() {
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
                this.log.debug(`ping failed for ${this.session.id}: ${error.message}`);
            }
        }, interval);
        this.pingTimer.unref?.();
    }

    /** Handle one inbound text message. */
    onMessage(data, isBinary) {
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

        envelope.received_at = new Date().toISOString();
        envelope.hub_sequence = this.session.stats.events + 1;
        envelope.issues = issues;
        envelope.connection = { opened_at: this.openedAt, protocol_errors: this.protocolErrors.length };
        this.lastEventAt = envelope.received_at;
        this.session.noteEnvelope(envelope);
        if (!envelope.known) {
            this.log.debug(`session ${this.session.id}: unknown event "${envelope.event}"`);
        }
        this.onEvent?.(envelope, this);
    }

    /** Update gap/duplicate counters from an event's sequence number. */
    trackSequence(sequence) {
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
    noteProtocolError(message, { fatal = true } = {}) {
        this.session.stats.protocolErrors += 1;
        this.protocolErrors.push({ at: new Date().toISOString(), message, fatal });
        this.log.warn(`session ${this.session.id}: protocol error: ${message}`);
        if (fatal && this.protocolErrors.length >= MAX_PROTOCOL_ERRORS) {
            this.close(1008, 'too many protocol errors');
        }
    }

    /** Socket error: logged, the close handler does the bookkeeping. */
    onError(error) {
        this.lastError = error.message;
        this.log.debug(`session ${this.session.id}: socket error: ${error.message}`);
    }

    /** Socket closed: detach from the session and tell the hub. */
    onClose(code, reasonBuffer) {
        clearInterval(this.pingTimer);
        this.pingTimer = null;
        this.closedAt = new Date().toISOString();
        const reason = reasonBuffer?.length ? reasonBuffer.toString('utf8') : '';
        this.closeReason ??= reason || `closed with code ${code}`;
        this.session.detach(this);
        this.log.info(`session ${this.session.id}: event connection closed (${code} ${this.closeReason})`);
        this.onClosed?.(this);
    }
}

/**
 * Build the worker-facing upgrade route for the hub's HTTP server.
 *
 * @param {object} options
 * @param {import('../state/registry.js').SessionRegistry} options.registry
 * @param {object} options.config
 * @param {object} options.log
 * @param {(envelope: object, connection: WorkerConnection) => void} [options.onEvent]
 * @param {(session: object, connection: WorkerConnection|null) => void} [options.onConnectionChange]
 * @returns {{match: Function, handle: Function, close: Function}}
 */
export function createWorkerEventRoute({ registry, config, log, onEvent, onConnectionChange }) {
    const wss = new WebSocketServer({
        noServer: true,
        maxPayload: config.limits.maxMessageBytes,
        perMessageDeflate: false,
    });

    function accept(ws, session) {
        const connection = new WorkerConnection({
            ws,
            session,
            config,
            log: log.child(`worker:${session.id}`),
            onEvent,
            onClosed: (closed) => {
                onConnectionChange?.(session, null);
                void closed;
            },
        });
        const { previous, replaced } = session.attach(connection);
        if (replaced && previous) {
            log.warn(`session ${session.id}: replacing an existing event connection`);
            previous.close(CLOSE_SUPERSEDED, 'superseded by a newer worker connection');
        }
        log.info(`session ${session.id}: event connection open`);
        onConnectionChange?.(session, connection);
    }

    return {
        match(req, url) {
            if (req.method !== 'GET') return null;
            const matched = EVENTS_ROUTE.exec(url.pathname);
            if (!matched) return null;
            try {
                return { session: decodeURIComponent(matched[1]) };
            } catch {
                return null;
            }
        },

        handle({ req, socket, head, url, params }) {
            const sessionId = params.session;
            if (!isValidSessionId(sessionId)) {
                log.warn(`rejected event upgrade: invalid session id "${sessionId}"`);
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

        close() {
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
