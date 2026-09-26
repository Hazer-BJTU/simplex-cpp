/**
 * @file one-shot tool-confirmation exchanges.
 *
 * Route: `GET /agent/<session_id>/confirm?token=<session token>`.
 *
 * Per core/docs/worker-protocol.md the worker opens one WebSocket per
 * confirmation, sends exactly one request, awaits exactly one response, and
 * then closes. Several confirmations may be in flight at once for one session,
 * so this route is deliberately independent of the event connection and of
 * event ordering.
 *
 * The one thing a confirmation cannot tell the hub by itself is *which worker
 * process* it belongs to. `worker_id` is self-declared, and the protocol
 * requires a mismatched one to be denied. The hub therefore compares it against
 * the identity observed on the session's event connection:
 *
 *   - identity live and matching      -> ask the operator,
 *   - identity live and different     -> deny (a stale incarnation, or a
 *     process that does not own this session),
 *   - identity unknown or stale       -> hold the prompt for a bounded window
 *     while the event connection identifies itself, then deny.
 *
 * A stale identity is never used for the comparison: a restarted worker has a
 * new `worker_id`, so comparing against the last known one would deny every
 * legitimate confirmation after a restart.
 */
import { WebSocketServer } from 'ws';
import type { RawData, WebSocket } from 'ws';
import type { Duplex } from 'node:stream';
import type { IncomingMessage } from 'node:http';
import { presentedToken, safeEqual } from '../http/auth.ts';
import { buildConfirmationResponse } from '../protocol/messages.ts';
import { IDENTITY } from '../state/registry.ts';
import type { Session, SessionRegistry, TrackedIdentity } from '../state/registry.ts';
import { isValidSessionId } from '../state/session-id.ts';
import { truncateReason } from './connection.ts';
import type { UpgradeContext, UpgradeHandler } from '../http/server.ts';
import type { HubConfig } from '../config.ts';
import type { Logger } from '../log.ts';
import type {
    ConfirmationOutcome,
    ConfirmationPrompt,
    PendingCall,
} from '../../shared/protocol.ts';

/** Upgrade path pattern for a one-shot confirmation connection. */
const CONFIRM_ROUTE = /^\/agent\/([^/]+)\/confirm$/;

/** Time allowed for the worker's close handshake after a decision is sent. */
const CLOSE_HANDSHAKE_MS = 3000;

/** Margin kept between the hub's own deadline and the worker's. */
const DEADLINE_MARGIN_MS = 5000;

/** Prompt lifecycle, as the panel sees it. */
export const PROMPT_STATE = {
    /** Waiting for the event connection to name its worker. */
    identity: 'awaiting-identity',
    /** Verified and waiting for an operator decision. */
    decision: 'awaiting-decision',
    /** A decision was sent to the worker. */
    decided: 'decided',
    /** Closed without an answer: deadline, disconnect, or hub shutdown. */
    retired: 'retired',
} as const;

/** One prompt state. */
export type PromptState = (typeof PROMPT_STATE)[keyof typeof PROMPT_STATE];

/** A decision, once the operator has made one. */
export interface DecisionOutcome {
    decision: 'approved' | 'denied';
    reason: string;
}

/** The outcome of judging a worker identity against a confirmation. */
export type IdentityVerdict =
    | { ok: true; held: boolean }
    | { ok: false; reason: string };

/** The result of trying to answer a prompt. */
export type DecideResult = { ok: true } | { ok: false; error: string };

/**
 * A confirmation request's `data`, after validation.
 *
 * `call` is what the worker is asking to do; the operator decides on it, so it
 * has to survive to the panel unharmed.
 */
export interface ConfirmationRequest {
    worker_id: string;
    session_id: string;
    run_id: string;
    confirmation_id: string;
    call: PendingCall;
}

/** Write a plain HTTP rejection on a socket that never became a WebSocket. */
function rejectUpgrade(socket: Duplex, status: number, text: string): void {
    socket.write(`HTTP/1.1 ${status} ${text}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
    socket.destroy();
}

/**
 * Wait until the session's live worker identity can judge a confirmation.
 *
 * @param session registry session.
 * @param workerId identity claimed by the request.
 * @param holdMs how long an unknown identity may be held.
 */
export function awaitWorkerIdentity(
    session: Session,
    workerId: string,
    holdMs: number,
): Promise<IdentityVerdict> {
    const judge = (identity: TrackedIdentity): IdentityVerdict | null => {
        if (identity.state !== IDENTITY.live) return null;
        if (identity.workerId === workerId) return { ok: true, held: false };
        return {
            ok: false,
            reason: `worker identity mismatch: the confirmation claims ${workerId} but the `
                + `event connection is ${String(identity.workerId)}`,
        };
    };

    const immediate = judge(session.identity);
    if (immediate) return Promise.resolve(immediate);
    if (holdMs <= 0) {
        return Promise.resolve({
            ok: false,
            reason: 'worker identity was not verified before the confirmation deadline',
        });
    }

    return new Promise<IdentityVerdict>((resolve) => {
        let timer: NodeJS.Timeout | null = null;
        let unsubscribe: () => void = () => {};
        const finish = (value: IdentityVerdict): void => {
            if (timer) clearTimeout(timer);
            unsubscribe();
            resolve(value);
        };
        timer = setTimeout(() => finish({
            ok: false,
            reason: `worker identity was not verified within ${holdMs} ms; the event connection `
                + 'did not identify the worker that opened this confirmation',
        }), holdMs);
        timer.unref?.();
        unsubscribe = session.onIdentityChange((identity) => {
            const verdict = judge(identity);
            if (!verdict) return;
            if (verdict.ok) verdict.held = true;
            finish(verdict);
        });
    });
}

/** Everything `new PendingConfirmation` needs. */
export interface PendingConfirmationOptions {
    request: ConfirmationRequest;
    session: Session;
    receivedAt: string;
    deadlineAt: string;
    log: Logger;
    onSettled?: ((prompt: PendingConfirmation, outcome: ConfirmationOutcome) => void) | undefined;
}

/** One open confirmation prompt, as seen by the panel. */
export class PendingConfirmation {
    readonly request: ConfirmationRequest;
    readonly session: Session;
    readonly receivedAt: string;
    readonly deadlineAt: string;
    readonly log: Logger;
    onSettled: ((prompt: PendingConfirmation, outcome: ConfirmationOutcome) => void)
        | null | undefined;
    state: PromptState;
    verified: boolean;
    decision: string | null;
    reason: string | null;
    settledAt: string | null;
    deadlineTimer: NodeJS.Timeout | null;
    private settle: ((value: DecisionOutcome | null) => void) | null;
    readonly finished: Promise<DecisionOutcome | null>;

    constructor({ request, session, receivedAt, deadlineAt, log, onSettled }: PendingConfirmationOptions) {
        this.request = request;
        this.session = session;
        this.receivedAt = receivedAt;
        this.deadlineAt = deadlineAt;
        this.log = log;
        this.onSettled = onSettled;
        this.state = PROMPT_STATE.identity;
        this.verified = false;
        this.decision = null;
        this.reason = null;
        this.settledAt = null;
        this.deadlineTimer = null;
        this.settle = null;
        this.finished = new Promise((resolve) => { this.settle = resolve; });
    }

    /** Worker-generated confirmation identifier. */
    get id(): string {
        return this.request.confirmation_id;
    }

    /** The settled call the worker is asking about. */
    get call(): PendingCall {
        return this.request.call ?? {};
    }

    /** Mark the prompt verified and let the operator answer it. */
    markVerified(): void {
        if (this.state !== PROMPT_STATE.identity) return;
        this.verified = true;
        this.state = PROMPT_STATE.decision;
    }

    /** Resolve when a decision is sent or the prompt is retired. */
    wait(): Promise<DecisionOutcome | null> {
        return this.finished;
    }

    /** Answer the prompt. */
    decide(decision: string, reason = 'operator decision'): DecideResult {
        if (this.state === PROMPT_STATE.decided) {
            return { ok: false, error: 'this confirmation was already answered' };
        }
        if (this.state === PROMPT_STATE.retired) {
            return { ok: false, error: 'this confirmation is no longer open' };
        }
        if (this.state !== PROMPT_STATE.decision) {
            return { ok: false, error: 'worker identity is not verified yet' };
        }
        if (decision !== 'approved' && decision !== 'denied') {
            return { ok: false, error: 'decision must be approved or denied' };
        }
        if (typeof reason !== 'string') {
            return { ok: false, error: 'reason must be a string' };
        }
        this.state = PROMPT_STATE.decided;
        this.decision = decision;
        this.reason = reason;
        this.settledAt = new Date().toISOString();
        this.clearDeadline();
        this.settle?.({ decision, reason });
        return { ok: true };
    }

    /**
     * Close the prompt without answering it.
     *
     * @param phase deadline|disconnected|shutdown|protocol-error
     * @param detail human-readable explanation for the panel.
     */
    retire(phase: string, detail: string): boolean {
        if (this.state === PROMPT_STATE.decided || this.state === PROMPT_STATE.retired) return false;
        this.state = PROMPT_STATE.retired;
        this.reason = detail;
        this.settledAt = new Date().toISOString();
        this.clearDeadline();
        this.settle?.(null);
        this.onSettled?.(this, { phase, detail });
        return true;
    }

    /** Arm the advisory deadline; the worker's own deadline started earlier. */
    armDeadline(afterMs: number, onExpired: () => void): void {
        this.deadlineTimer = setTimeout(onExpired, afterMs);
        this.deadlineTimer.unref?.();
    }

    /** Stop the advisory deadline timer. */
    clearDeadline(): void {
        if (this.deadlineTimer) clearTimeout(this.deadlineTimer);
        this.deadlineTimer = null;
    }

    /** Serializable description for the panel. */
    describe(): ConfirmationPrompt {
        return {
            confirmation_id: this.id,
            session_id: this.session.id,
            worker_id: this.request.worker_id,
            run_id: this.request.run_id,
            state: this.state,
            verified: this.verified,
            identity_state: this.session.identity.state,
            call: this.call,
            received_at: this.receivedAt,
            deadline_at: this.deadlineAt,
            settled_at: this.settledAt,
            decision: this.decision,
            reason: this.reason,
        };
    }
}

/** One inbound message, or null when the socket closed first. */
interface ReceivedMessage {
    data: RawData;
    isBinary: boolean;
}

/** A validated request, or the reason it was refused. */
type ReadRequestResult =
    | { ok: true; request: ConfirmationRequest }
    | { ok: false; error: string };

/** Everything `createWorkerConfirmationRoute` needs. */
export interface ConfirmationRouteOptions {
    registry: SessionRegistry;
    config: HubConfig;
    log: Logger;
    /** Called with a verified prompt, once the operator can answer it. */
    onPrompt?: ((prompt: PendingConfirmation) => void) | undefined;
    /** Called when a prompt settles, for any reason. */
    onSettled?: ((prompt: PendingConfirmation, outcome: ConfirmationOutcome) => void) | undefined;
}

/** The confirmation route, as a WebSocket upgrade handler. */
export interface ConfirmationRoute extends UpgradeHandler {
    close(): void;
}

/** Build the confirmation upgrade route. */
export function createWorkerConfirmationRoute({
    registry, config, log, onPrompt, onSettled,
}: ConfirmationRouteOptions): ConfirmationRoute {
    const wss = new WebSocketServer({
        noServer: true,
        maxPayload: config.limits.maxMessageBytes,
        perMessageDeflate: false,
    });
    const open = new Set<WebSocket>();

    /** Read one text message, or null when the socket closes first. */
    function nextMessage(ws: WebSocket): Promise<ReceivedMessage | null> {
        return new Promise((resolve) => {
            const onMessage = (data: RawData, isBinary: boolean): void => {
                ws.off('close', onClose);
                resolve({ data, isBinary });
            };
            const onClose = (): void => {
                ws.off('message', onMessage);
                resolve(null);
            };
            ws.once('message', onMessage);
            ws.once('close', onClose);
        });
    }

    /** Write exactly one decision to the connection. */
    function sendDecision(
        ws: WebSocket,
        request: ConfirmationRequest,
        decision: string,
        reason: string,
        slog: Logger,
    ): boolean {
        if (ws.readyState !== ws.OPEN) return false;
        const response = buildConfirmationResponse(request, decision as 'approved' | 'denied',
            truncateReason(reason));
        try {
            ws.send(JSON.stringify(response));
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            slog.warn(`could not send a decision: ${message}`);
            ws.terminate();
            return false;
        }
        slog.info(`confirmation ${request.confirmation_id}: ${decision}`);
        return true;
    }

    /**
     * Wait for the close handshake the worker starts after reading a decision.
     *
     * The observer is notified *before* this runs: whether the transport
     * finishes closing must not delay what the panel is told.
     */
    async function completeClose(ws: WebSocket): Promise<void> {
        if (ws.readyState === ws.CLOSED) return;
        const closed = new Promise((resolve) => ws.once('close', resolve));
        const timer = setTimeout(() => {
            if (ws.readyState !== ws.CLOSED) ws.terminate();
        }, CLOSE_HANDSHAKE_MS);
        timer.unref?.();
        await closed;
        clearTimeout(timer);
    }

    /** Reject a malformed or duplicate request without answering it. */
    function abort(ws: WebSocket, slog: Logger, message: string): void {
        slog.warn(`confirmation aborted: ${message}`);
        try {
            ws.close(1008, truncateReason(message));
        } catch {
            ws.terminate();
        }
    }

    /** Validate a received confirmation request. */
    function readRequest(text: string | null, session: Session): ReadRequestResult {
        if (typeof text !== 'string') {
            return { ok: false, error: 'binary confirmation request' };
        }
        let parsed: unknown;
        try {
            parsed = JSON.parse(text);
        } catch (cause) {
            const message = cause instanceof Error ? cause.message : String(cause);
            return { ok: false, error: `invalid JSON: ${message}` };
        }
        if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
            return { ok: false, error: 'confirmation request must be a JSON object' };
        }
        const document = parsed as Record<string, unknown>;
        if (document.type !== 'confirmation_request') {
            return {
                ok: false,
                error: `expected confirmation_request, got ${JSON.stringify(document.type)}`,
            };
        }
        const data = document.data;
        if (typeof data !== 'object' || data === null || Array.isArray(data)) {
            return { ok: false, error: 'confirmation request data must be an object' };
        }
        const fields = data as Record<string, unknown>;
        for (const field of ['worker_id', 'session_id', 'run_id', 'confirmation_id'] as const) {
            if (typeof fields[field] !== 'string' || (fields[field] as string).length === 0) {
                return { ok: false, error: `confirmation ${field} must be a nonempty string` };
            }
        }
        if (fields.session_id !== session.id) {
            return {
                ok: false,
                error: `confirmation session_id "${String(fields.session_id)}" does not match route session`,
            };
        }
        if (typeof fields.call !== 'object' || fields.call === null || Array.isArray(fields.call)) {
            return { ok: false, error: 'confirmation call must be an object' };
        }
        if (session.prompts.has(fields.confirmation_id as string)) {
            return { ok: false, error: `duplicate confirmation_id ${String(fields.confirmation_id)}` };
        }
        return {
            ok: true,
            request: {
                worker_id: fields.worker_id as string,
                session_id: fields.session_id as string,
                run_id: fields.run_id as string,
                confirmation_id: fields.confirmation_id as string,
                call: fields.call as PendingCall,
            },
        };
    }

    /** Run one exchange to completion. */
    async function exchange(ws: WebSocket, session: Session): Promise<void> {
        const slog = log.child(`confirm:${session.id}`);
        let prompt: PendingConfirmation | null = null;
        let settled = false;
        const cleanup = (phase: string, detail: string): void => {
            if (settled) return;
            settled = true;
            if (prompt) {
                session.removePrompt(prompt);
                prompt.clearDeadline();
                prompt.retire(phase, detail);
            }
        };

        try {
            const first = await nextMessage(ws);
            if (!first) return;
            const text = first.isBinary ? null : first.data.toString('utf8');
            const parsed = readRequest(text, session);
            if (!parsed.ok) {
                session.stats.protocolErrors += 1;
                abort(ws, slog, parsed.error);
                return;
            }
            const request = parsed.request;

            // A second application frame violates the single-request contract.
            ws.on('message', () => {
                session.stats.protocolErrors += 1;
                abort(ws, slog, 'more than one application frame on a confirmation connection');
            });

            const receivedAt = Date.now();
            const deadlineMs = config.worker.confirmationTimeoutMs;
            const deadlineAt = new Date(receivedAt + deadlineMs).toISOString();
            const holdMs = Math.max(
                0,
                Math.min(config.limits.confirmIdentityHoldMs, deadlineMs - DEADLINE_MARGIN_MS));

            const verdict = await awaitWorkerIdentity(session, request.worker_id, holdMs);
            if (!verdict.ok) {
                slog.warn(`denying ${request.confirmation_id}: ${verdict.reason}`);
                sendDecision(ws, request, 'denied', verdict.reason, slog);
                await completeClose(ws);
                return;
            }

            prompt = new PendingConfirmation({
                request,
                session,
                receivedAt: new Date(receivedAt).toISOString(),
                deadlineAt,
                log: slog,
                onSettled: (settledPrompt, outcome) => onSettled?.(settledPrompt, outcome),
            });
            prompt.markVerified();
            session.addPrompt(prompt);
            const closed = new Promise<null>((resolve) => ws.once('close', () => resolve(null)));
            const remainingMs = Math.max(0, deadlineMs - (Date.now() - receivedAt));
            prompt.armDeadline(remainingMs, () => {
                // Advisory: the worker's own deadline started before this
                // connection existed, so a missed local deadline only retires
                // the prompt — the worker has already denied on its own.
                cleanup('deadline', 'the confirmation deadline expired before a decision');
                if (ws.readyState === ws.OPEN) ws.close(1001, 'confirmation deadline');
            });
            slog.info(`confirmation ${prompt.id} awaiting a decision (worker ${request.worker_id})`);
            onPrompt?.(prompt);

            const outcome = await Promise.race([prompt.wait(), closed]);
            if (outcome === null) {
                cleanup('disconnected', 'the worker closed the confirmation connection');
                return;
            }
            prompt.onSettled = null;
            settled = true;
            sendDecision(ws, request, outcome.decision, outcome.reason, slog);
            onSettled?.(prompt, { phase: 'decided', detail: outcome.decision });
            await completeClose(ws);
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            slog.warn(`confirmation exchange failed: ${message}`);
            try {
                ws.terminate();
            } catch { /* already gone */ }
        } finally {
            cleanup('disconnected', 'the confirmation connection closed');
            if (prompt) session.removePrompt(prompt);
        }
    }

    function accept(ws: WebSocket, session: Session): void {
        open.add(ws);
        ws.once('close', () => open.delete(ws));
        void exchange(ws, session);
    }

    return {
        match(req: IncomingMessage, url: URL): Record<string, unknown> | null {
            if (req.method !== 'GET') return null;
            const matched = CONFIRM_ROUTE.exec(url.pathname);
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
                log.warn(`rejected confirmation upgrade: invalid session id "${String(sessionId)}"`);
                rejectUpgrade(socket, 404, 'Not Found');
                return;
            }
            const session = registry.get(sessionId);
            if (!session) {
                log.warn(`rejected confirmation upgrade: unknown session "${sessionId}"`);
                rejectUpgrade(socket, 404, 'Not Found');
                return;
            }
            if (!safeEqual(presentedToken(url), session.token)) {
                log.warn(`rejected confirmation upgrade for ${sessionId}: bad token`);
                rejectUpgrade(socket, 401, 'Unauthorized');
                return;
            }
            wss.handleUpgrade(req, socket, head, (ws) => accept(ws, session));
        },

        close(): void {
            for (const session of registry.list()) {
                for (const prompt of [...session.prompts.values()]) {
                    (prompt as PendingConfirmation).retire('shutdown', 'the hub is shutting down');
                    session.removePrompt(prompt);
                }
            }
            for (const ws of open) {
                try {
                    ws.close(1001, 'hub shutting down');
                } catch {
                    ws.terminate();
                }
            }
            wss.close();
        },
    };
}
