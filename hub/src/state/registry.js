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
 */
import { randomBytes, randomUUID } from 'node:crypto';
import { validateSessionId } from './session-id.js';

/** A fresh per-session access token. */
export function newToken() {
    return randomBytes(24).toString('base64url');
}

/** A fresh worker-independent identifier for hub-created sessions. */
export function newSessionId(prefix = 'session') {
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
};

/**
 * One session: deployment facts plus live connection state.
 *
 * The identity is deliberately three-valued. A stale identity must never be
 * used to judge a confirmation, because the worker it names may be gone and a
 * restarted worker has a new `worker_id`; see src/worker/confirmation.js.
 */
export class Session {
    constructor({ id, spec = {}, token = newToken(), log }) {
        validateSessionId(id);
        this.id = id;
        this.token = token;
        this.spec = spec;
        this.log = log;
        this.createdAt = new Date().toISOString();

        /** Currently attached event connection, or null. */
        this.connection = null;
        /** Last event connection that delivered this session's events. */
        this.lastConnectionAt = null;
        this.identity = { state: IDENTITY.unknown, workerId: null, since: null, lastWorkerId: null };
        this.stats = {
            events: 0,
            gaps: 0,
            duplicates: 0,
            protocolErrors: 0,
            incarnations: 0,
        };
        /** Most recent envelopes by name, for cheap panel snapshots. */
        this.latest = { status: null, options: null, run_finished: null };
        /** Last event envelope seen, whatever its name. */
        this.lastEvent = null;
        /** Run identifier the hub most recently observed, for cancel targeting. */
        this.lastRunId = '';
        /** Open tool-confirmation prompts, keyed by confirmation ID. */
        this.prompts = new Map();
        /** Identity observers, used by the confirmation adapter's hold. */
        this.identityListeners = new Set();
    }

    /** True while an event connection is open. */
    get connected() {
        return this.connection !== null && this.connection.isOpen;
    }

    /**
     * Attach a new event connection, detaching (and reporting) any previous one.
     * @returns {{previous: object|null, replaced: boolean}}
     */
    attach(connection) {
        const previous = this.connection;
        this.connection = connection;
        this.lastConnectionAt = new Date().toISOString();
        return { previous, replaced: previous !== null && previous !== connection };
    }

    /** Detach a connection if it is still the current one. */
    detach(connection) {
        if (this.connection !== connection) return false;
        this.connection = null;
        this.markStale();
        return true;
    }

    /**
     * Record the worker instance that just identified itself.
     * @returns {{incarnation: boolean}} whether this is a new worker process.
     */
    noteIdentity(workerId, at = new Date().toISOString()) {
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
    markStale() {
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
     * itself instead of polling; listeners are synchronous and must not throw.
     *
     * @param {(identity: object) => void} listener
     * @returns {() => void} unsubscribe
     */
    onIdentityChange(listener) {
        this.identityListeners.add(listener);
        return () => this.identityListeners.delete(listener);
    }

    /** Notify identity observers, containing their failures to this call. */
    notifyIdentity() {
        for (const listener of [...this.identityListeners]) {
            try {
                listener(this.identity);
            } catch (error) {
                this.log?.warn(`identity listener failed: ${error.message}`);
            }
        }
    }

    /** Register an open confirmation prompt. */
    addPrompt(prompt) {
        this.prompts.set(prompt.id, prompt);
    }

    /** Remove a confirmation prompt if it is still registered. */
    removePrompt(prompt) {
        if (this.prompts.get(prompt.id) === prompt) this.prompts.delete(prompt.id);
    }

    /** Serializable descriptions of open prompts, oldest first. */
    describePrompts() {
        return [...this.prompts.values()].map((prompt) => prompt.describe());
    }

    /** Record an inbound envelope and update the cheap panel snapshots. */
    noteEnvelope(envelope) {
        this.stats.events += 1;
        this.lastEvent = envelope;
        if (Object.hasOwn(this.latest, envelope.event)) this.latest[envelope.event] = envelope;
        if (typeof envelope.run_id === 'string' && envelope.run_id.length > 0) {
            this.lastRunId = envelope.run_id;
        }
    }

    /** Serializable description for the panel. */
    describe() {
        return {
            session_id: this.id,
            created_at: this.createdAt,
            spec: this.spec,
            connected: this.connected,
            identity: {
                state: this.identity.state,
                worker_id: this.identity.workerId ?? this.identity.lastWorkerId,
                since: this.identity.since,
            },
            stats: { ...this.stats },
            last_run_id: this.lastRunId,
            last_event_at: this.lastEvent?.received_at ?? null,
            last_event: this.lastEvent?.event ?? null,
            confirmations: this.describePrompts(),
        };
    }
}

/** Error carrying an HTTP status, so routes can map it directly. */
export class NotFoundError extends Error {
    constructor(message) {
        super(message);
        this.name = 'NotFoundError';
        this.status = 404;
    }
}

/** Holds every session the hub knows about, keyed by session ID. */
export class SessionRegistry {
    constructor({ config, log }) {
        this.config = config;
        this.log = log;
        this.sessions = new Map();
    }

    /** Create a session; throws when the identifier is taken. */
    create(id, spec = {}) {
        validateSessionId(id);
        if (this.sessions.has(id)) throw new Error(`session "${id}" already exists`);
        const session = new Session({ id, spec, log: this.log });
        this.sessions.set(id, session);
        this.log.info(`session created: ${id}`);
        return session;
    }

    /** Create the session when absent, otherwise return the existing one. */
    ensure(id, spec = {}) {
        const existing = this.get(id);
        if (existing) return existing;
        return this.create(id, spec);
    }

    /** Look up a session, or undefined. */
    get(id) {
        return this.sessions.get(id);
    }

    /** Look up a session, or throw NotFoundError. */
    require(id) {
        const session = this.get(id);
        if (!session) throw new NotFoundError(`unknown session "${id}"`);
        return session;
    }

    /** Every session, oldest first. */
    list() {
        return [...this.sessions.values()];
    }

    /** Remove a session from the registry. */
    remove(id) {
        return this.sessions.delete(id);
    }
}
