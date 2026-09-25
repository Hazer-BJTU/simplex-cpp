/**
 * @file panel transport: token storage, REST helper, panel WebSocket client.
 *
 * The panel speaks the hub's own protocol (hub/src/panel/api.js): REST for
 * metadata, snapshots and a session-list fallback, and one WebSocket at
 * `/panel/ws` for everything live. This module is deliberately DOM-free at
 * import time so it can be loaded by plain Node for a syntax/load check:
 * globals are read lazily inside functions and may be injected for tests.
 */

/** Panel protocol version carried by every message. */
export const PANEL_VERSION = 1;

/** localStorage key holding an accepted panel token. */
export const TOKEN_KEY = 'simplex-hub-token';

/** Reconnect backoff bounds for the panel socket. */
export const BACKOFF_START_MS = 500;
export const BACKOFF_MAX_MS = 10000;

/** Error carrying the HTTP status and the hub's machine-readable code. */
export class ApiError extends Error {
    constructor(message, { status = 0, code = 'network_error', body = null } = {}) {
        super(message);
        this.name = 'ApiError';
        this.status = status;
        this.code = code;
        this.body = body;
    }

    /** True when the hub rejected the request for a missing/wrong token. */
    get unauthorized() {
        return this.status === 401;
    }
}

/** A storage lookalike backed by a plain Map, for environments without one. */
function memoryStorage() {
    const map = new Map();
    return {
        getItem: (key) => (map.has(key) ? map.get(key) : null),
        setItem: (key, value) => { map.set(key, String(value)); },
        removeItem: (key) => { map.delete(key); },
    };
}

/** localStorage when the browser exposes a usable one, else memory. */
export function safeStorage(storage) {
    if (storage) return storage;
    try {
        const candidate = globalThis.localStorage;
        if (candidate) {
            const probe = '__simplex_hub_probe__';
            candidate.setItem(probe, '1');
            candidate.removeItem(probe);
            return candidate;
        }
    } catch {
        // Private mode or a blocked origin: keep the token in memory only.
    }
    return memoryStorage();
}

/** Token present in a page URL (`?token=...`), or '' when absent. */
export function tokenFromUrl(location) {
    try {
        const value = location?.search ? new URLSearchParams(location.search).get('token') : null;
        return typeof value === 'string' ? value : '';
    } catch {
        return '';
    }
}

/** Rewrite the address bar without the token query parameter. */
export function stripTokenFromUrl(location, history) {
    try {
        if (!location || !history) return;
        const url = new URL(location.href);
        if (!url.searchParams.has('token')) return;
        url.searchParams.delete('token');
        history.replaceState(null, '', `${url.pathname}${url.search}${url.hash}`);
    } catch {
        // A failed rewrite only leaves the token in the address bar; never fatal.
    }
}

/**
 * Token holder: URL token wins over stored token, and a URL token is persisted
 * and removed from the address bar. The value is never rendered anywhere.
 */
export function createTokenStore({ storage, location, history } = {}) {
    const store = safeStorage(storage);
    const loc = location ?? globalThis.location ?? null;
    const hist = history ?? globalThis.history ?? null;
    let token = '';
    try {
        token = store.getItem(TOKEN_KEY) ?? '';
    } catch {
        token = '';
    }
    const fromUrl = tokenFromUrl(loc);
    if (fromUrl) {
        token = fromUrl;
        try {
            store.setItem(TOKEN_KEY, token);
        } catch { /* memory-only fallback */ }
        stripTokenFromUrl(loc, hist);
    }
    return {
        get: () => token,
        has: () => token.length > 0,
        set(value) {
            token = typeof value === 'string' ? value : '';
            try {
                if (token) store.setItem(TOKEN_KEY, token);
                else store.removeItem(TOKEN_KEY);
            } catch { /* memory-only fallback */ }
        },
        clear() {
            this.set('');
        },
    };
}

/** WebSocket URL for the panel socket, with the token in the query string. */
export function panelSocketUrl({ location, token } = {}) {
    const loc = location ?? globalThis.location;
    const protocol = loc?.protocol === 'https:' ? 'wss:' : 'ws:';
    const host = loc?.host ?? '127.0.0.1';
    const url = new URL(`${protocol}//${host}/panel/ws`);
    if (token) url.searchParams.set('token', token);
    return url.toString();
}

/**
 * REST helper for the hub's JSON API.
 *
 * @param {object} [options]
 * @param {string} [options.base] origin prefix; '' means same origin.
 * @param {() => string} [options.token] current panel token.
 * @param {typeof fetch} [options.fetchImpl]
 * @param {() => void} [options.onUnauthorized] called once per 401.
 */
export function createRest({ base = '', token = () => '', fetchImpl, onUnauthorized } = {}) {
    const doFetch = fetchImpl ?? globalThis.fetch;

    async function request(method, path, { body, query } = {}) {
        if (typeof doFetch !== 'function') {
            throw new ApiError('fetch is not available in this environment', { code: 'no_fetch' });
        }
        const url = new URL(`${base}${path}`, globalThis.location?.href ?? 'http://127.0.0.1/');
        for (const [key, value] of Object.entries(query ?? {})) {
            if (value !== undefined && value !== null) url.searchParams.set(key, String(value));
        }
        const headers = { Accept: 'application/json' };
        const presented = token();
        if (presented) headers.Authorization = `Bearer ${presented}`;
        const init = { method, headers };
        if (body !== undefined) {
            headers['Content-Type'] = 'application/json';
            init.body = JSON.stringify(body);
        }
        let response;
        try {
            response = await doFetch(url.toString(), init);
        } catch (error) {
            throw new ApiError(`cannot reach the hub: ${error.message}`, { code: 'network_error' });
        }
        const text = await response.text();
        let parsed = null;
        if (text.length > 0) {
            try {
                parsed = JSON.parse(text);
            } catch {
                parsed = null;
            }
        }
        if (!response.ok) {
            const code = typeof parsed?.error === 'string' ? parsed.error : `http_${response.status}`;
            const message = typeof parsed?.message === 'string' ? parsed.message : response.statusText;
            if (response.status === 401) onUnauthorized?.();
            throw new ApiError(message || code, { status: response.status, code, body: parsed });
        }
        return parsed;
    }

    return {
        request,
        meta: () => request('GET', '/api/meta'),
        sessions: () => request('GET', '/api/sessions'),
        session: (id) => request('GET', `/api/sessions/${encodeURIComponent(id)}`),
        createSession: (session, spec) => request('POST', '/api/sessions', { body: { session, spec } }),
        deleteSession: (id) => request('DELETE', `/api/sessions/${encodeURIComponent(id)}`),
        worker: (id, action, spec) => request('POST',
            `/api/sessions/${encodeURIComponent(id)}/${action}`, { body: spec === undefined ? {} : { spec } }),
        events: (id, since = 0, limit = 0) => request('GET',
            `/api/sessions/${encodeURIComponent(id)}/events`, { query: { since, limit } }),
        logs: (id, limit = 200) => request('GET',
            `/api/sessions/${encodeURIComponent(id)}/logs`, { query: { limit } }),
        snapshot: (id) => request('GET', `/api/sessions/${encodeURIComponent(id)}/snapshot`),
    };
}

/**
 * Panel WebSocket client with capped exponential backoff.
 *
 * States reported through `onState`:
 *   connecting -> open -> (reconnecting|rejected) -> ... -> closed
 * `rejected` means the socket closed before any `welcome`, which is what a
 * token rejection looks like from inside a browser (no HTTP status is
 * available); the caller decides whether that means authentication.
 *
 * @param {object} [options]
 * @param {() => string} [options.token]
 * @param {(message: object|null, raw: string) => void} [options.onMessage]
 * @param {(state: object) => void} [options.onState]
 */
export function createPanelSocket({
    location, token = () => '', onMessage, onState,
    WebSocketImpl, startDelayMs = BACKOFF_START_MS, maxDelayMs = BACKOFF_MAX_MS,
    setTimeoutImpl, clearTimeoutImpl, random = Math.random,
} = {}) {
    const Impl = WebSocketImpl ?? globalThis.WebSocket;
    const setTimer = setTimeoutImpl ?? globalThis.setTimeout;
    const clearTimer = clearTimeoutImpl ?? globalThis.clearTimeout;
    const loc = location ?? globalThis.location ?? null;
    let socket = null;
    let timer = null;
    let attempt = 0;
    let sawWelcome = false;
    let intentionalClose = false;
    let current = 'idle';

    function report(state, extra = {}) {
        current = state;
        onState?.({ state, attempt, nextDelayMs: extra.nextDelayMs ?? null, ...extra });
    }

    function delayFor(nextAttempt) {
        const raw = startDelayMs * (2 ** Math.max(0, nextAttempt - 1));
        const capped = Math.min(raw, maxDelayMs);
        // A little jitter keeps two open panels from reconnecting in lockstep.
        return Math.round(capped * (0.9 + random() * 0.2));
    }

    function scheduleReconnect(rejected) {
        const nextDelayMs = delayFor(attempt + 1);
        report(rejected ? 'rejected' : 'reconnecting', { nextDelayMs, welcomeReceived: sawWelcome });
        timer = setTimer(() => {
            timer = null;
            open();
        }, nextDelayMs);
    }

    function open() {
        if (!Impl) {
            report('closed', { error: 'WebSocket is not available' });
            return;
        }
        intentionalClose = false;
        report('connecting');
        let created;
        try {
            created = new Impl(panelSocketUrl({ location: loc, token: token() }));
        } catch (error) {
            report('closed', { error: error.message });
            return;
        }
        socket = created;
        created.onopen = () => {
            attempt = 0;
            report('open');
        };
        created.onmessage = (event) => {
            const raw = typeof event?.data === 'string' ? event.data : '';
            let parsed = null;
            try {
                parsed = JSON.parse(raw);
            } catch {
                parsed = null;
            }
            if (parsed && parsed.type === 'welcome') {
                sawWelcome = true;
                attempt = 0;
            }
            onMessage?.(parsed, raw);
        };
        created.onerror = () => {
            // The close handler owns reconnection; browsers give no detail here.
        };
        created.onclose = () => {
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
        /** Drop the current socket and reconnect immediately (token changed). */
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
        /**
         * Send one versioned panel message.
         * @returns {boolean} false when the socket is not open (nothing was sent).
         */
        send(message) {
            if (!socket || socket.readyState !== 1) return false;
            try {
                socket.send(JSON.stringify({ v: PANEL_VERSION, ...message }));
                return true;
            } catch {
                return false;
            }
        },
        isOpen: () => Boolean(socket) && socket.readyState === 1,
        state: () => current,
    };
}
