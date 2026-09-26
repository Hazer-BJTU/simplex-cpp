/**
 * @file the panel's socket to the hub.
 *
 * One WebSocket at `/panel/ws` carries everything live: the session list,
 * transcript envelopes, confirmations, process state. This module owns the
 * connection and nothing else — it parses, it reports, it reconnects with a
 * capped backoff, and it never touches the store. That separation is what lets
 * the store be tested without a socket and the socket be tested without a DOM.
 *
 * Two behaviours carried over from `web/js/api.js` because they were right:
 *
 * - A URL token is read once, at connect time, so rotating the token is a
 *   `reconnectNow()` rather than a special case inside every send.
 * - "Closed before any `welcome`" is reported as `rejected` rather than as an
 *   ordinary disconnect. A browser cannot see the HTTP status of a failed
 *   WebSocket handshake, so this is the only signal that distinguishes "the hub
 *   is not running" from "the hub does not accept this token".
 *
 * What is new: inbound frames go through `checkHubEnvelope` from the shared
 * module, so an unfamiliar message type is ignored deliberately (the protocol's
 * forward-compatibility rule) and a version mismatch is reported as itself
 * rather than showing up as a render error somewhere downstream.
 */
import { checkHubEnvelope } from '../../../shared/guards.ts';
import { PANEL_VERSION, type ErrorCode, type HubMessage, type PanelMessage } from '../../../shared/protocol.ts';
import type { LocationLike } from './token.ts';

/** Reconnect backoff bounds. */
export const BACKOFF_START_MS = 500;
export const BACKOFF_MAX_MS = 10_000;

/**
 * Connection state.
 *
 * `rejected` is a close that happened before any `welcome`; the caller decides
 * whether that means authentication, because only it knows whether a token was
 * presented.
 */
export type PanelSocketState =
    | 'idle' | 'connecting' | 'open' | 'reconnecting' | 'rejected' | 'closed';

/** What the socket reports about itself, on every state change. */
export interface PanelSocketStatus {
    state: PanelSocketState;
    /** Consecutive failed attempts; reset by a successful open. */
    attempt: number;
    /** Delay before the next attempt, or null when none is scheduled. */
    nextDelayMs: number | null;
    /** True once a `welcome` has arrived on the current connection. */
    welcomeReceived: boolean;
    /** Set when the socket could not even be constructed. */
    error?: string | undefined;
}

/** Something arrived from the hub. */
export type PanelSocketEvent =
    /** A message this panel understands. */
    | { kind: 'message'; message: HubMessage; raw: string }
    /** A type this protocol version does not define: ignored by design. */
    | { kind: 'ignored'; type: string }
    /** The frame could not be used at all. */
    | { kind: 'refused'; code: ErrorCode; detail: string };

/** WebSocket URL for the panel socket, with the token in the query string. */
export function panelSocketUrl(options: {
    location?: LocationLike | null;
    token?: string;
} = {}): string {
    const loc = options.location ?? (globalThis.location as LocationLike | undefined);
    const protocol = loc?.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = new URL(`${protocol}//${loc?.host ?? '127.0.0.1'}/panel/ws`);
    if (options.token) url.searchParams.set('token', options.token);
    return url.toString();
}

/** Everything `createPanelSocket` needs. */
export interface PanelSocketOptions {
    location?: LocationLike | null;
    /** The token to present; read once per connection attempt. */
    token?: () => string;
    onEvent?: (event: PanelSocketEvent) => void;
    onState?: (status: PanelSocketStatus) => void;
    /** Injected for tests; defaults to the global `WebSocket`. */
    WebSocketImpl?: typeof WebSocket;
    startDelayMs?: number;
    maxDelayMs?: number;
    setTimeoutImpl?: typeof setTimeout;
    clearTimeoutImpl?: typeof clearTimeout;
    random?: () => number;
}

/** The panel's live connection. */
export interface PanelSocket {
    connect(): void;
    /** Drop the current socket and reconnect immediately (the token changed). */
    reconnectNow(): void;
    close(): void;
    /** Send one versioned panel message; false when nothing was sent. */
    send(message: PanelMessage): boolean;
    isOpen(): boolean;
    state(): PanelSocketState;
}

export function createPanelSocket(options: PanelSocketOptions = {}): PanelSocket {
    const Impl = options.WebSocketImpl ?? globalThis.WebSocket;
    const setTimer = options.setTimeoutImpl ?? globalThis.setTimeout;
    const clearTimer = options.clearTimeoutImpl ?? globalThis.clearTimeout;
    const random = options.random ?? Math.random;
    const loc = options.location ?? (globalThis.location as LocationLike | undefined) ?? null;
    const startDelayMs = options.startDelayMs ?? BACKOFF_START_MS;
    const maxDelayMs = options.maxDelayMs ?? BACKOFF_MAX_MS;
    const token = options.token ?? (() => '');

    let socket: WebSocket | null = null;
    let timer: ReturnType<typeof setTimeout> | null = null;
    let attempt = 0;
    let sawWelcome = false;
    let intentionalClose = false;
    let current: PanelSocketState = 'idle';

    function report(state: PanelSocketState, extra: Partial<PanelSocketStatus> = {}): void {
        current = state;
        options.onState?.({
            state,
            attempt,
            nextDelayMs: null,
            welcomeReceived: sawWelcome,
            ...extra,
        });
    }

    function delayFor(nextAttempt: number): number {
        const raw = startDelayMs * (2 ** Math.max(0, nextAttempt - 1));
        const capped = Math.min(raw, maxDelayMs);
        // A little jitter keeps two open panels from reconnecting in lockstep.
        return Math.round(capped * (0.9 + random() * 0.2));
    }

    function scheduleReconnect(rejected: boolean): void {
        const nextDelayMs = delayFor(attempt + 1);
        report(rejected ? 'rejected' : 'reconnecting', { nextDelayMs });
        timer = setTimer(() => {
            timer = null;
            open();
        }, nextDelayMs);
    }

    function open(): void {
        if (!Impl) {
            report('closed', { error: 'WebSocket is not available' });
            return;
        }
        intentionalClose = false;
        sawWelcome = false;
        report('connecting');
        let created: WebSocket;
        try {
            created = new Impl(panelSocketUrl({ location: loc, token: token() }));
        } catch (error) {
            report('closed', {
                error: error instanceof Error ? error.message : String(error),
            });
            return;
        }
        socket = created;
        created.onopen = () => {
            if (socket !== created) return;
            attempt = 0;
            report('open');
        };
        created.onmessage = (event: MessageEvent) => {
            if (socket !== created) return;
            const raw = typeof event.data === 'string' ? event.data : '';
            const check = checkHubEnvelope(raw);
            if (check.kind === 'unknown_type') {
                options.onEvent?.({ kind: 'ignored', type: check.type });
                return;
            }
            if (check.kind === 'rejected') {
                options.onEvent?.({ kind: 'refused', code: check.code, detail: check.detail });
                return;
            }
            if (check.message.type === 'welcome') {
                sawWelcome = true;
                attempt = 0;
            }
            options.onEvent?.({ kind: 'message', message: check.message, raw });
        };
        created.onerror = () => {
            // The close handler owns reconnection; browsers give no detail here.
        };
        created.onclose = () => {
            // Closing the old socket during a token change must not clear the
            // replacement or schedule a second reconnect.
            if (socket !== created) return;
            socket = null;
            if (intentionalClose) {
                report('closed');
                return;
            }
            attempt += 1;
            scheduleReconnect(!sawWelcome);
        };
    }

    return {
        connect() {
            if (socket || timer) return;
            sawWelcome = false;
            open();
        },
        reconnectNow() {
            intentionalClose = true;
            if (timer) {
                clearTimer(timer);
                timer = null;
            }
            attempt = 0;
            sawWelcome = false;
            const previous = socket;
            socket = null;
            try {
                previous?.close();
            } catch { /* already gone */ }
            open();
        },
        close() {
            intentionalClose = true;
            if (timer) {
                clearTimer(timer);
                timer = null;
            }
            const previous = socket;
            socket = null;
            try {
                previous?.close();
            } catch { /* already gone */ }
            report('closed');
        },
        send(message) {
            if (!socket || socket.readyState !== 1) return false;
            try {
                socket.send(JSON.stringify({ v: PANEL_VERSION, ...message }));
                return true;
            } catch {
                return false;
            }
        },
        isOpen: () => socket !== null && socket.readyState === 1,
        state: () => current,
    };
}
