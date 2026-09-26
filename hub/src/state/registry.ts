/**
 * @file the hub's session registry.
 *
 * A session is the unit the worker protocol is built around: one worker process
 * owns one `session_id`, persists one conversation, and executes at most one
 * agent-loop invocation at a time. The hub keeps the deployment-side facts a
 * single worker cannot know — its launch specification, its access token, which
 * process is running it, and who is watching it — and the live facts the panel
 * needs to render it.
 *
 * State changes here are synchronous: the hub runs one event loop, so there is
 * no lock in this file by design.
 *
 * `describe()` is where this module meets the panel: it returns the
 * `SessionDescription` from `shared/protocol.ts`, so the contract the browser
 * reads and the object the hub builds are the same type rather than two things
 * that are expected to agree.
 */
import { randomBytes, randomUUID } from 'node:crypto';
import { validateSessionId } from './session-id.ts';
import type { Logger } from '../log.ts';
import type {
    ConfirmationPrompt,
    ProcessDescription,
    RequestRecord,
    SessionDescription,
    SessionIdentity,
    SessionSpec,
} from '../../shared/protocol.ts';

/** A fresh per-session access token. */
export function newToken(): string {
    return randomBytes(24).toString('base64url');
}

/** A fresh worker-independent identifier for hub-created sessions. */
export function newSessionId(prefix = 'session'): string {
    return `${prefix}-${randomUUID().slice(0, 8)}`;
}

/** Identity state of the worker believed to own a session. */
export const IDENTITY = {
    /** No event connection has delivered an event since the hub started. */
    unknown: 'unknown',
    /** An event connection is open and has identified itself. */
    live: 'live',
    /** The identity was observed but the connection is currently closed. */
    stale: 'stale',
} as const;

/** One identity state. */
export type IdentityState = (typeof IDENTITY)[keyof typeof IDENTITY];

/** The identity, with the internal `lastWorkerId` the panel does not see. */
export interface TrackedIdentity {
    state: IdentityState;
    workerId: string | null;
    since: string | null;
    /** The last worker seen, kept after a disconnect so the panel can name it. */
    lastWorkerId: string | null;
}

/** Per-session counters. */
export interface RegistryStats {
    events: number;
    gaps: number;
    duplicates: number;
    protocolErrors: number;
    incarnations: number;
}

/** The event names the registry keeps a cheap latest-copy of. */
export const LATEST_EVENT_NAMES = ['status', 'options', 'run_finished'] as const;

/** Most recent envelopes by name, for cheap panel snapshots. */
export type LatestEnvelopes = Record<(typeof LATEST_EVENT_NAMES)[number], RegistryEnvelope | null>;

/** An event envelope, as far as the registry cares. */
export interface RegistryEnvelope {
    event: string;
    run_id?: unknown;
    received_at?: unknown;
    [field: string]: unknown;
}

/** The slice of a worker event connection the registry holds. */
export interface AttachedConnection {
    readonly isOpen: boolean;
}

/** The slice of a supervised process record the registry holds. */
export interface AttachedProcess {
    pid: number | null;
    describe(): ProcessDescription;
}

/** The slice of a confirmation prompt the registry holds. */
export interface RegisteredPrompt {
    id: string;
    describe(): ConfirmationPrompt;
}

/** Everything `new Session` needs. */
export interface SessionOptions {
    id: string;
    spec?: SessionSpec;
    token?: string;
    log?: Logger;
}

/**
 * One session: deployment facts plus live connection state.
 *
 * The identity is deliberately three-valued. A stale identity must never be
 * used to judge a confirmation, because the worker it names may be gone and a
 * restarted worker has a new `worker_id`; see src/worker/confirmation.ts.
 */
export class Session {
    readonly id: string;
    readonly token: string;
    spec: SessionSpec;
    readonly log: Logger | undefined;
    readonly createdAt: string;

    /** Currently attached event connection, or null. */
    connection: AttachedConnection | null;
    /** Last time an event connection was attached. */
    lastConnectionAt: string | null;
    identity: TrackedIdentity;
    stats: RegistryStats;
    latest: LatestEnvelopes;
    /** Last event envelope seen, whatever its name. */
    lastEvent: RegistryEnvelope | null;
    /** Run identifier the hub most recently observed, for cancel targeting. */
    lastRunId: string;
    /** Open tool-confirmation prompts, keyed by confirmation ID. */
    readonly prompts: Map<string, RegisteredPrompt>;
    /** Identity observers, used by the confirmation adapter's hold. */
    readonly identityListeners: Set<(identity: TrackedIdentity) => void>;
    /** Supervised worker process record, or null. */
    process: AttachedProcess | null;
    /**
     * Outcomes of payloads this hub sent, keyed by request_id.
     *
     * The worker protocol has no delivery acknowledgement: a successful send
     * only means the message reached the socket. Until admission or rejection
     * is observed, the outcome is genuinely unknown, and the hub must preserve
     * that instead of retrying side-effecting work.
     */
    readonly requests: Map<string, RequestRecord>;

    constructor({ id, spec = {}, token = newToken(), log }: SessionOptions) {
        validateSessionId(id);
        this.id = id;
        this.token = token;
        this.spec = spec;
        this.log = log;
        this.createdAt = new Date().toISOString();

        this.connection = null;
        this.lastConnectionAt = null;
        this.identity = {
            state: IDENTITY.unknown, workerId: null, since: null, lastWorkerId: null,
        };
        this.stats = { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 0 };
        this.latest = { status: null, options: null, run_finished: null };
        this.lastEvent = null;
        this.lastRunId = '';
        this.prompts = new Map();
        this.identityListeners = new Set();
        this.process = null;
        this.requests = new Map();
    }

    /** True while an event connection is open. */
    get connected(): boolean {
        return this.connection !== null && this.connection.isOpen;
    }

    /**
     * Attach a new event connection, detaching (and reporting) any previous one.
     */
    attach(connection: AttachedConnection): { previous: AttachedConnection | null; replaced: boolean } {
        const previous = this.connection;
        this.connection = connection;
        this.lastConnectionAt = new Date().toISOString();
        return { previous, replaced: previous !== null && previous !== connection };
    }

    /** Detach a connection if it is still the current one. */
    detach(connection: AttachedConnection): boolean {
        if (this.connection !== connection) return false;
        this.connection = null;
        this.markStale();
        this.markRequestsUnknown();
        return true;
    }

    /** Record a payload the hub just sent. */
    trackRequest(requestId: string, operation: string): RequestRecord {
        const entry: RequestRecord = {
            request_id: requestId,
            operation,
            state: 'sent',
            sent_at: new Date().toISOString(),
            settled_at: null,
            detail: '',
        };
        this.requests.set(requestId, entry);
        this.pruneRequests();
        return entry;
    }

    /** Mark a tracked request admitted. */
    noteRequestAdmitted(requestId: string): boolean {
        const entry = this.requests.get(requestId);
        if (!entry || entry.state !== 'sent') return false;
        entry.state = 'admitted';
        entry.settled_at = new Date().toISOString();
        return true;
    }

    /** Mark a tracked request rejected, with the worker's diagnostic. */
    noteRequestRejected(requestId: string, detail: string | null | undefined): boolean {
        const entry = this.requests.get(requestId);
        if (!entry || entry.state !== 'sent') return false;
        entry.state = 'rejected';
        entry.detail = detail ?? '';
        entry.settled_at = new Date().toISOString();
        return true;
    }

    /**
     * Mark every in-flight request unknown.
     *
     * Called when the event connection drops: the payload may or may not have
     * been admitted, and only the operator can decide what to do next.
     */
    markRequestsUnknown(): void {
        const now = new Date().toISOString();
        for (const entry of this.requests.values()) {
            if (entry.state !== 'sent') continue;
            entry.state = 'unknown';
            entry.settled_at = now;
            entry.detail = 'the event connection closed before admission was observed';
        }
    }

    /** Keep the request map bounded, preferring to forget settled entries. */
    pruneRequests(limit = 200): void {
        if (this.requests.size <= limit) return;
        for (const [key, entry] of this.requests) {
            if (this.requests.size <= limit) break;
            if (entry.state === 'sent') continue;
            this.requests.delete(key);
        }
    }

    /** Tracked requests, most recent last. */
    describeRequests(): RequestRecord[] {
        return [...this.requests.values()];
    }

    /**
     * Record the worker instance that just identified itself.
     *
     * @returns whether this is a new worker process.
     */
    noteIdentity(workerId: string, at = new Date().toISOString()): { incarnation: boolean } {
        const previous = this.identity.workerId;
        const incarnation = previous !== null && previous !== workerId;
        if (incarnation) this.stats.incarnations += 1;
        this.identity = {
            state: IDENTITY.live,
            workerId,
            since: incarnation || previous === null ? at : this.identity.since,
            lastWorkerId: workerId,
        };
        this.notifyIdentity();
        return { incarnation };
    }

    /** Mark a known identity as no longer backed by an open connection. */
    markStale(): void {
        if (this.identity.state !== IDENTITY.live) return;
        this.identity = {
            state: IDENTITY.stale,
            workerId: null,
            since: null,
            lastWorkerId: this.identity.workerId,
        };
        this.notifyIdentity();
    }

    /**
     * Observe identity transitions.
     *
     * The confirmation adapter uses this to wait for a worker to identify
     * itself instead of polling; listeners are synchronous and must not throw,
     * which `notifyIdentity` enforces rather than trusting.
     */
    onIdentityChange(listener: (identity: TrackedIdentity) => void): () => void {
        this.identityListeners.add(listener);
        return () => { this.identityListeners.delete(listener); };
    }

    /** Notify identity observers, containing their failures to this call. */
    notifyIdentity(): void {
        for (const listener of [...this.identityListeners]) {
            try {
                listener(this.identity);
            } catch (error) {
                const message = error instanceof Error ? error.message : String(error);
                this.log?.warn(`identity listener failed: ${message}`);
            }
        }
    }

    /** Register an open confirmation prompt. */
    addPrompt(prompt: RegisteredPrompt): void {
        this.prompts.set(prompt.id, prompt);
    }

    /** Remove a confirmation prompt if it is still registered. */
    removePrompt(prompt: RegisteredPrompt): void {
        if (this.prompts.get(prompt.id) === prompt) this.prompts.delete(prompt.id);
    }

    /** Serializable descriptions of open prompts, oldest first. */
    describePrompts(): ConfirmationPrompt[] {
        return [...this.prompts.values()].map((prompt) => prompt.describe());
    }

    /** Record an inbound envelope and update the cheap panel snapshots. */
    noteEnvelope(envelope: RegistryEnvelope): void {
        this.stats.events += 1;
        this.lastEvent = envelope;
        if (Object.hasOwn(this.latest, envelope.event)) {
            (this.latest as Record<string, RegistryEnvelope | null>)[envelope.event] = envelope;
        }
        if (typeof envelope.run_id === 'string' && envelope.run_id.length > 0) {
            this.lastRunId = envelope.run_id;
        }
    }

    /** Serializable description for the panel. */
    describe(): SessionDescription {
        const identity: SessionIdentity = {
            state: this.identity.state,
            worker_id: this.identity.workerId ?? this.identity.lastWorkerId,
            since: this.identity.since,
        };
        return {
            session_id: this.id,
            created_at: this.createdAt,
            spec: this.spec,
            connected: this.connected,
            identity,
            stats: { ...this.stats },
            last_run_id: this.lastRunId,
            last_event_at: typeof this.lastEvent?.received_at === 'string'
                ? this.lastEvent.received_at
                : null,
            last_event: this.lastEvent?.event ?? null,
            confirmations: this.describePrompts(),
            process: this.process?.describe() ?? null,
            requests: this.describeRequests(),
        };
    }
}

/** Error carrying an HTTP status, so routes can map it directly. */
export class NotFoundError extends Error {
    readonly status = 404;
    constructor(message: string) {
        super(message);
        this.name = 'NotFoundError';
    }
}

/** The slice of hub configuration the registry reads. */
export interface RegistryConfig {
    [key: string]: unknown;
}

/** Everything `SessionRegistry` needs. */
export interface RegistryOptions {
    config: RegistryConfig;
    log: Logger;
}

/** Holds every session the hub knows about, keyed by session ID. */
export class SessionRegistry {
    readonly config: RegistryConfig;
    readonly log: Logger;
    readonly sessions: Map<string, Session>;

    constructor({ config, log }: RegistryOptions) {
        this.config = config;
        this.log = log;
        this.sessions = new Map();
    }

    /** Create a session; throws when the identifier is taken. */
    create(id: string, spec: SessionSpec = {}): Session {
        validateSessionId(id);
        if (this.sessions.has(id)) throw new Error(`session "${id}" already exists`);
        const session = new Session({ id, spec, log: this.log });
        this.sessions.set(id, session);
        this.log.info(`session created: ${id}`);
        return session;
    }

    /** Create the session when absent, otherwise return the existing one. */
    ensure(id: string, spec: SessionSpec = {}): Session {
        const existing = this.get(id);
        if (existing) return existing;
        return this.create(id, spec);
    }

    /** Look up a session, or undefined. */
    get(id: string): Session | undefined {
        return this.sessions.get(id);
    }

    /** Look up a session, or throw NotFoundError. */
    require(id: string): Session {
        const session = this.get(id);
        if (!session) throw new NotFoundError(`unknown session "${id}"`);
        return session;
    }

    /** Every session, oldest first. */
    list(): Session[] {
        return [...this.sessions.values()];
    }

    /** Remove a session from the registry. */
    remove(id: string): boolean {
        return this.sessions.delete(id);
    }
}
