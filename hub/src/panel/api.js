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
 */
import { existsSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { WebSocketServer } from 'ws';
import { authorizePanel, presentedToken, safeEqual } from '../http/auth.ts';
import { readJsonBody, sendError, sendJson } from '../http/server.js';
import { persistenceRoot } from '../launch/config-render.js';
import { normalizeSpec } from '../launch/spec.js';
import { buildPayload, buildSignal, newRequestId } from '../protocol/messages.ts';
import { isValidSessionId } from '../state/session-id.ts';
import { checkEnvelope } from '../../shared/guards.ts';
import { PANEL_VERSION, SESSIONLESS_MESSAGE_TYPES } from '../../shared/protocol.ts';

// The version lives in `shared/protocol.ts` so the hub and the panel cannot
// disagree about it. Re-exported because this module is where a reader expects
// to find it, and because that is the name every message is stamped with.
export { PANEL_VERSION };

/** Largest on-disk artifact the snapshot viewer will read. */
const SNAPSHOT_MAX_BYTES = 8 * 1024 * 1024;

/** Log lines returned by one explicit logs request. */
const LOG_TAIL_DEFAULT = 200;
const LOG_TAIL_MAX = 2000;

/**
 * Build the panel API: REST routes, the panel WebSocket, and the observer hooks
 * the rest of the hub reports through.
 *
 * @param {object} options
 * @param {object} options.config hub configuration.
 * @param {object} options.log hub logger.
 * @param {object} options.registry session registry.
 * @param {object} options.supervisor worker supervisor.
 * @param {object} options.transcripts transcript store.
 * @param {object} options.state durable hub state.
 * @param {() => object} options.meta metadata for `welcome` and `/api/meta`.
 * @param {(sessions: object[]) => void} [options.onSessionsChanged] persistence hook.
 */
export function createPanelApi({
    config, log, registry, supervisor, transcripts, state, meta, onSessionsChanged,
}) {
    const clients = new Set();
    const wss = new WebSocketServer({
        noServer: true,
        maxPayload: 4 * 1024 * 1024,
        perMessageDeflate: false,
    });

    /** Send one versioned message to a panel client. */
    function send(client, message) {
        if (client.ws.readyState !== client.ws.OPEN) return;
        client.ws.send(JSON.stringify({ v: PANEL_VERSION, ...message }));
    }

    /** Send to every client subscribed to a session. */
    function broadcastToSession(sessionId, message) {
        for (const client of clients) {
            if (client.subscriptions.has(sessionId)) send(client, message);
        }
    }

    /** Send to every connected panel client. */
    function broadcast(message) {
        for (const client of clients) send(client, message);
    }

    /** Broadcast the current description of one session. */
    function broadcastSession(session) {
        broadcast({ type: 'session', session: session.describe() });
    }

    /**
     * Persist the session list after a change.
     *
     * Process changes are written immediately: adoption after a hub restart
     * depends on the recorded pid, and a worker that starts and outlives a
     * crash must not be lost to a pending debounce.
     */
    function persist({ immediate = false } = {}) {
        const sessions = registry.list();
        if (immediate) state.flush(sessions);
        else onSessionsChanged?.(sessions);
    }

    // ---------------------------------------------------------------------
    // Observer hooks
    // ---------------------------------------------------------------------

    const hooks = {
        onEvent: (envelope, connection) => {
            const session = connection.session;
            transcripts.get(session.id).append(envelope);
            if (envelope.event === 'input_admitted') {
                if (session.noteRequestAdmitted(envelope.request_id)) {
                    broadcastToSession(session.id, {
                        type: 'request',
                        session: session.id,
                        request: session.requests.get(envelope.request_id),
                    });
                }
            } else if (envelope.event === 'input_rejected') {
                const requestId = envelope.data?.request_id;
                if (typeof requestId === 'string' && session.noteRequestRejected(requestId, envelope.data?.message)) {
                    broadcastToSession(session.id, {
                        type: 'request',
                        session: session.id,
                        request: session.requests.get(requestId),
                    });
                }
            }
            broadcastToSession(session.id, {
                type: 'event',
                session: session.id,
                hub_seq: envelope.hub_sequence,
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
                },
            });
            broadcastSession(session);
        },
    };

    // ---------------------------------------------------------------------
    // Shared helpers
    // ---------------------------------------------------------------------

    /** Load a session or send a 404. */
    function requireSession(res, sessionId) {
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

    /** Guard a REST request with panel authentication. */
    function authorized(req, url) {
        return authorizePanel(config, req, url).ok;
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
     * @param {unknown} rawSpec the `spec` field as the client sent it.
     * @returns {string|null} an error message, or null when the spec is usable.
     */
    function checkSpec(rawSpec) {
        if (rawSpec === undefined || rawSpec === null) return null;
        if (typeof rawSpec !== 'object' || Array.isArray(rawSpec)) {
            return 'spec must be a JSON object';
        }
        try {
            normalizeSpec(config, rawSpec);
            return null;
        } catch (error) {
            return error.message;
        }
    }

    /** Read the tail of a session's on-disk snapshot, read-only. */
    function readSnapshot(session) {
        const directory = join(persistenceRoot(config), session.id);
        const statePath = join(directory, 'state.json');
        const readablePath = join(directory, 'readable.md');
        const result = { session_id: session.id, state: null, readable: null, files: {} };
        if (existsSync(statePath) && statSync(statePath).size <= SNAPSHOT_MAX_BYTES) {
            result.files.state = statePath;
            try {
                result.state = JSON.parse(readFileSync(statePath, 'utf8'));
            } catch (error) {
                result.state_error = error.message;
            }
        }
        if (existsSync(readablePath) && statSync(readablePath).size <= SNAPSHOT_MAX_BYTES) {
            result.files.readable = readablePath;
            result.readable = readFileSync(readablePath, 'utf8');
        }
        return result;
    }

    /** Build and send a payload on behalf of the panel. */
    function sendInput(session, body) {
        const connection = session.connection;
        if (!connection?.isOpen) return { ok: false, error: 'the worker is not connected' };
        const requestId = typeof body.request_id === 'string' && body.request_id.length > 0
            ? body.request_id
            : newRequestId();
        let payload;
        try {
            payload = buildPayload({
                operation: body.operation ?? 'message',
                requestId,
                content: body.content,
                options: body.options,
            });
        } catch (error) {
            return { ok: false, error: error.message };
        }
        const entry = session.trackRequest(payload.data.request_id, payload.data.operation);
        const sent = connection.sendPayload(payload);
        if (!sent.ok) {
            session.requests.delete(payload.data.request_id);
            return { ok: false, error: sent.error };
        }
        broadcastToSession(session.id, { type: 'request', session: session.id, request: entry });
        return { ok: true, request_id: payload.data.request_id, payload };
    }

    /** Build and send a signal on behalf of the panel. */
    function sendSignal(session, body) {
        const connection = session.connection;
        if (!connection?.isOpen) return { ok: false, error: 'the worker is not connected' };
        const runId = body.run_id ?? (body.operation === 'cancel' ? session.lastRunId : undefined);
        let signal;
        try {
            signal = buildSignal({ operation: body.operation, runId });
        } catch (error) {
            return { ok: false, error: error.message };
        }
        const sent = connection.sendSignal(signal);
        return sent.ok ? { ok: true } : { ok: false, error: sent.error };
    }

    /** Apply a worker action requested by the panel. */
    async function workerAction(session, action, spec) {
        switch (action) {
            case 'start':
                return supervisor.start(session, spec);
            case 'stop':
                return supervisor.stop(session);
            case 'restart':
                return supervisor.restart(session, spec);
            case 'force-kill':
                return supervisor.forceKill(session);
            default:
                return { ok: false, error: `unknown worker action "${action}"` };
        }
    }

    // ---------------------------------------------------------------------
    // REST
    // ---------------------------------------------------------------------

    const routes = {
        'GET /api/sessions': ({ res }) => {
            sendJson(res, 200, {
                sessions: registry.list().map((session) => session.describe()),
            });
        },

        'POST /api/sessions': async ({ req, res, body }) => {
            const parsed = await readJsonBody(req, config.limits.maxMessageBytes);
            const id = parsed.session;
            if (!isValidSessionId(id)) {
                sendError(res, 400, 'invalid_session', 'session id must be 1-128 [A-Za-z0-9_-]');
                return;
            }
            if (registry.get(id)) {
                sendError(res, 409, 'session_exists', `session "${id}" already exists`);
                return;
            }
            const rawSpec = parsed.spec ?? {};
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
                sendError(res, 400, 'invalid_session', error.message);
            }
            void body;
        },

        'GET /api/sessions/:id': ({ res, params }) => {
            const session = requireSession(res, params.id);
            if (!session) return;
            sendJson(res, 200, { session: session.describe() });
        },

        'DELETE /api/sessions/:id': ({ res, params }) => {
            const session = requireSession(res, params.id);
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
            const session = requireSession(res, params.id);
            if (!session) return;
            const parsed = await readJsonBody(req, config.limits.maxMessageBytes);
            const result = await supervisor.start(session, parsed.spec);
            persist();
            broadcastSession(session);
            sendJson(res, result.ok ? 200 : 409, result);
        },

        'POST /api/sessions/:id/stop': async ({ res, params }) => {
            const session = requireSession(res, params.id);
            if (!session) return;
            const result = await supervisor.stop(session);
            persist();
            broadcastSession(session);
            sendJson(res, 200, result);
        },

        'POST /api/sessions/:id/restart': async ({ req, res, params }) => {
            const session = requireSession(res, params.id);
            if (!session) return;
            const parsed = await readJsonBody(req, config.limits.maxMessageBytes);
            const result = await supervisor.restart(session, parsed.spec);
            persist();
            broadcastSession(session);
            sendJson(res, result.ok ? 200 : 409, result);
        },

        'POST /api/sessions/:id/force-kill': async ({ res, params }) => {
            const session = requireSession(res, params.id);
            if (!session) return;
            const result = await supervisor.forceKill(session);
            persist();
            broadcastSession(session);
            sendJson(res, 200, result);
        },

        'GET /api/sessions/:id/events': ({ res, url, params }) => {
            const session = requireSession(res, params.id);
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
            const session = requireSession(res, params.id);
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
            const session = requireSession(res, params.id);
            if (!session) return;
            sendJson(res, 200, readSnapshot(session));
        },
    };

    // ---------------------------------------------------------------------
    // Panel WebSocket
    // ---------------------------------------------------------------------

    /** Handle one panel message. */
    async function handleMessage(client, raw) {
        // The envelope check is shared with the panel (`shared/guards.ts`): the
        // same code decides what a valid frame is at both ends, so a message the
        // hub refuses is one the panel never meant to send.
        const check = checkEnvelope(raw.toString('utf8'));
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
        const sessionId = message.session;
        const session = sessionId === undefined ? null : registry.get(sessionId);

        const needsSession = !SESSIONLESS_MESSAGE_TYPES.includes(type);
        if (needsSession && !session) {
            send(client, {
                type: 'error',
                error: 'unknown_session',
                message: `unknown session "${sessionId}"`,
                request: message,
            });
            return;
        }

        switch (type) {
            case 'ping':
                send(client, { type: 'pong', at: new Date().toISOString() });
                return;
            case 'list_sessions':
                send(client, { type: 'sessions', sessions: registry.list().map((s) => s.describe()) });
                return;
            case 'subscribe': {
                if (!session) {
                    send(client, {
                        type: 'error',
                        error: 'unknown_session',
                        message: `unknown session "${sessionId}"`,
                        request: message,
                    });
                    return;
                }
                client.subscriptions.add(session.id);
                send(client, {
                    type: 'subscribed',
                    session: session.describe(),
                    transcript: transcripts.get(session.id).since(Number(message.since) || 0),
                    logs: supervisor.logs(session, { limit: LOG_TAIL_DEFAULT }),
                    latest: transcripts.get(session.id).sequence,
                    // Echoed so a client can tell "nothing new" apart from "your
                    // cursor predates a restart, and this hub's sequence started
                    // over". Without it the second case looks exactly like the
                    // first and the transcript just appears empty.
                    transcript_epoch: meta().transcript_epoch,
                });
                return;
            }
            case 'unsubscribe':
                client.subscriptions.delete(sessionId);
                return;
            case 'create_session': {
                if (!isValidSessionId(sessionId)) {
                    send(client, {
                        type: 'error', error: 'invalid_session',
                        message: 'session id must be 1-128 [A-Za-z0-9_-]', request: message,
                    });
                    return;
                }
                if (registry.get(sessionId)) {
                    send(client, {
                        type: 'error', error: 'session_exists',
                        message: `session "${sessionId}" already exists`, request: message,
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
                const created = registry.create(sessionId, message.spec ?? {});
                created.spec = message.spec ?? {};
                persist();
                broadcastSession(created);
                send(client, { type: 'created', session: created.describe() });
                return;
            }
            case 'delete_session': {
                if (supervisor.isRunning(session) || session.connected) {
                    send(client, {
                        type: 'error', error: 'session_busy',
                        message: 'stop the worker and disconnect it before deleting the session',
                        request: message,
                    });
                    return;
                }
                transcripts.remove(session.id);
                registry.remove(session.id);
                client.subscriptions.delete(session.id);
                persist();
                broadcast({ type: 'session_removed', session: session.id });
                return;
            }
            case 'worker': {
                const result = await workerAction(session, message.action, message.spec);
                persist();
                broadcastSession(session);
                send(client, {
                    type: result.ok ? 'accepted' : 'error',
                    ...(result.ok ? {} : { error: 'worker_action_failed' }),
                    action: message.action,
                    session: session.id,
                    result,
                    message: result.error,
                });
                return;
            }
            case 'input': {
                const result = sendInput(session, message);
                if (!result.ok) {
                    send(client, {
                        type: 'error', error: 'input_not_sent',
                        message: result.error, request: message,
                    });
                    return;
                }
                send(client, {
                    type: 'accepted', action: 'input', session: session.id,
                    request_id: result.request_id,
                });
                return;
            }
            case 'signal': {
                const result = sendSignal(session, message);
                if (!result.ok) {
                    send(client, {
                        type: 'error', error: 'signal_not_sent',
                        message: result.error, request: message,
                    });
                    return;
                }
                send(client, {
                    type: 'accepted', action: 'signal',
                    operation: message.operation, session: session.id,
                });
                return;
            }
            case 'confirmation': {
                const prompt = session.prompts.get(message.confirmation_id);
                if (!prompt) {
                    send(client, {
                        type: 'error', error: 'unknown_confirmation',
                        message: 'that confirmation is no longer open', request: message,
                    });
                    return;
                }
                const result = prompt.decide(message.decision, message.reason ?? 'operator decision');
                send(client, {
                    type: result.ok ? 'accepted' : 'error',
                    ...(result.ok ? {} : { error: 'confirmation_rejected' }),
                    action: 'confirmation',
                    session: session.id,
                    confirmation_id: message.confirmation_id,
                    message: result.error,
                });
                return;
            }
            case 'logs': {
                const limit = Math.min(Number(message.limit) || LOG_TAIL_DEFAULT, LOG_TAIL_MAX);
                send(client, {
                    type: 'logs',
                    session: session.id,
                    lines: supervisor.logs(session, { limit }),
                    dropped: session.process?.logs?.dropped ?? 0,
                });
                return;
            }
            case 'status_snapshot': {
                send(client, {
                    type: 'snapshot',
                    session: session.describe(),
                    transcript: transcripts.get(session.id).since(Number(message.since) || 0),
                });
                return;
            }
            default:
                log.debug(`panel: ignoring unknown message type ${JSON.stringify(type)}`);
        }
    }

    function accept(ws, req) {
        const client = {
            ws,
            subscriptions: new Set(),
            openedAt: new Date().toISOString(),
            remote: req.socket.remoteAddress,
        };
        clients.add(client);
        ws.on('message', (data, isBinary) => {
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
            handleMessage(client, data).catch((error) => {
                log.error(`panel message failed: ${error.message}`, error);
                send(client, {
                    type: 'error', error: 'internal_error',
                    message: 'the hub failed to process that message',
                });
            });
        });
        ws.on('close', () => clients.delete(client));
        ws.on('error', (error) => log.debug(`panel socket error: ${error.message}`));
        send(client, {
            type: 'welcome',
            hub: meta(),
            sessions: registry.list().map((session) => session.describe()),
            subscriptions: [],
        });
    }

    const upgrade = {
        match(req, url) {
            if (req.method !== 'GET' || url.pathname !== '/panel/ws') return null;
            return {};
        },
        handle({ req, socket, head, url }) {
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
            const origin = req.headers.origin;
            if (typeof origin === 'string' && origin.length > 0) {
                let originHost = null;
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
        /** Number of connected panel clients, for diagnostics and tests. */
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
