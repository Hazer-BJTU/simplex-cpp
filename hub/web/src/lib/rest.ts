/**
 * @file the panel's HTTP client.
 *
 * Everything the panel needs that is not a live stream: hub metadata, the
 * session list, a snapshot, a log tail. The socket is the primary source; this
 * module exists so a panel can still show something when the socket cannot
 * connect, and so a lost transcript has a way back (the old panel had four
 * unused REST methods and therefore no such path at all).
 *
 * The response types come from `shared/protocol.ts`, the same file the hub
 * reads, so a renamed field is a compile error here instead of an `undefined`
 * that renders as a blank panel.
 */
import type {
    ApiErrorBody,
    EventsResponse,
    HubMetadata,
    LogsResponse,
    RemovedResponse,
    SessionDescription,
    SessionId,
    SessionListResponse,
    SessionResponse,
    SessionSpec,
    SnapshotView,
    SupervisorResult,
} from '../../../shared/protocol.ts';

/** One process action the hub exposes over REST. */
export type WorkerAction = 'start' | 'stop' | 'restart' | 'force-kill';

/** Query parameters; a null or undefined value is omitted from the URL. */
export type Query = Record<string, string | number | boolean | null | undefined>;

/** Error carrying the HTTP status and the hub's machine-readable code. */
export class ApiError extends Error {
    readonly status: number;
    readonly code: string;
    readonly body: ApiErrorBody | null;

    constructor(message: string, options: {
        status?: number;
        code?: string;
        body?: ApiErrorBody | null;
    } = {}) {
        super(message);
        this.name = 'ApiError';
        this.status = options.status ?? 0;
        this.code = options.code ?? 'network_error';
        this.body = options.body ?? null;
    }

    /** True when the hub rejected the request for a missing or wrong token. */
    get unauthorized(): boolean {
        return this.status === 401;
    }
}

/** Everything `createRest` needs. */
export interface RestOptions {
    /** Origin prefix; `''` means same origin. */
    base?: string;
    /** The token to present, read on every call. */
    token?: () => string;
    /** Injected for tests; defaults to the global `fetch`. */
    fetchImpl?: typeof fetch;
    /** Called once per 401, before the error is thrown. */
    onUnauthorized?: () => void;
}

/** The hub's JSON API. */
export interface RestClient {
    request<T>(method: string, path: string, options?: {
        body?: unknown;
        query?: Query;
    }): Promise<T>;
    meta(): Promise<HubMetadata>;
    sessions(): Promise<SessionListResponse>;
    session(id: SessionId): Promise<SessionResponse>;
    createSession(session: SessionId, spec?: SessionSpec): Promise<SessionResponse>;
    deleteSession(id: SessionId): Promise<RemovedResponse>;
    worker(id: SessionId, action: WorkerAction, spec?: SessionSpec): Promise<SupervisorResult>;
    events(id: SessionId, since?: number, limit?: number): Promise<EventsResponse>;
    logs(id: SessionId, limit?: number): Promise<LogsResponse>;
    snapshot(id: SessionId): Promise<SnapshotView>;
}

/** Read a response body as JSON, tolerating an empty or non-JSON one. */
async function readJson(response: Response): Promise<unknown> {
    const text = await response.text();
    if (text.length === 0) return null;
    try {
        return JSON.parse(text);
    } catch {
        return null;
    }
}

/** Narrow an unknown body to the error shape, or null. */
function asErrorBody(value: unknown): ApiErrorBody | null {
    if (typeof value !== 'object' || value === null) return null;
    const body = value as Record<string, unknown>;
    if (typeof body.error !== 'string') return null;
    return {
        error: body.error,
        message: typeof body.message === 'string' ? body.message : body.error,
        details: body.details,
    };
}

/**
 * Create the REST client.
 *
 * `base` is empty by default: the panel is served from the hub's own origin, so
 * a relative URL is correct there and in `vite dev`, where the dev server
 * proxies `/api` to the hub. One origin means no CORS and no second place for
 * the token to be accepted.
 */
export function createRest(options: RestOptions = {}): RestClient {
    const base = options.base ?? '';
    const token = options.token ?? (() => '');
    const doFetch = options.fetchImpl ?? globalThis.fetch;

    async function request<T>(
        method: string,
        path: string,
        call: { body?: unknown; query?: Query } = {},
    ): Promise<T> {
        if (typeof doFetch !== 'function') {
            throw new ApiError('fetch is not available in this environment', { code: 'no_fetch' });
        }
        // Relative to the document when there is one, and to a fixed origin when
        // there is not, so the URL is always absolute before `fetch` sees it.
        const href = globalThis.location?.href ?? 'http://127.0.0.1/';
        const url = new URL(`${base}${path}`, href);
        for (const [key, value] of Object.entries(call.query ?? {})) {
            if (value !== null && value !== undefined) url.searchParams.set(key, String(value));
        }
        const headers: Record<string, string> = { Accept: 'application/json' };
        const presented = token();
        if (presented) headers.Authorization = `Bearer ${presented}`;
        const init: RequestInit = { method, headers };
        if (call.body !== undefined) {
            headers['Content-Type'] = 'application/json';
            init.body = JSON.stringify(call.body);
        }

        let response: Response;
        try {
            response = await doFetch(url.toString(), init);
        } catch (error) {
            const detail = error instanceof Error ? error.message : String(error);
            throw new ApiError(`cannot reach the hub: ${detail}`, { code: 'network_error' });
        }

        const parsed = await readJson(response);
        if (!response.ok) {
            const body = asErrorBody(parsed);
            if (response.status === 401) options.onUnauthorized?.();
            throw new ApiError(body?.message ?? response.statusText ?? `http_${response.status}`, {
                status: response.status,
                code: body?.error ?? `http_${response.status}`,
                body,
            });
        }
        return parsed as T;
    }

    return {
        request,
        meta: () => request<HubMetadata>('GET', '/api/meta'),
        sessions: () => request<SessionListResponse>('GET', '/api/sessions'),
        session: (id) => request<SessionResponse>('GET', `/api/sessions/${encodeURIComponent(id)}`),
        createSession: (session, spec) => request<SessionResponse>('POST', '/api/sessions', {
            body: spec === undefined ? { session } : { session, spec },
        }),
        deleteSession: (id) => request<RemovedResponse>(
            'DELETE', `/api/sessions/${encodeURIComponent(id)}`),
        worker: (id, action, spec) => request<SupervisorResult>(
            'POST',
            `/api/sessions/${encodeURIComponent(id)}/${action}`,
            { body: spec === undefined ? {} : { spec } },
        ),
        events: (id, since = 0, limit = 0) => request<EventsResponse>(
            'GET',
            `/api/sessions/${encodeURIComponent(id)}/events`,
            { query: { since, limit } },
        ),
        logs: (id, limit = 200) => request<LogsResponse>(
            'GET',
            `/api/sessions/${encodeURIComponent(id)}/logs`,
            { query: { limit } },
        ),
        snapshot: (id) => request<SnapshotView>(
            'GET', `/api/sessions/${encodeURIComponent(id)}/snapshot`),
    };
}

/** Re-exported so callers do not have to import from two protocol modules. */
export type { SessionDescription };
