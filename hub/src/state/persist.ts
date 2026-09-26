/**
 * @file hub-owned durable state.
 *
 * The hub stores only what it cannot reconstruct: which sessions exist, their
 * launch specifications and access tokens, and enough process identity to
 * decide whether a worker recorded by a previous hub run is still alive. Tokens
 * must survive a restart, otherwise every running worker would be locked out by
 * its own hub; process records include `/proc` start time so a reused pid can
 * never be mistaken for the original worker.
 *
 * Conversation history is deliberately absent: that is the worker's snapshot
 * (`<persistence.directory>/<session>/state.json`), and duplicating it here
 * would create a second source of truth.
 */
import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import type { Logger } from '../log.ts';

/** Bumped when the stored shape changes incompatibly. */
export const STATE_VERSION = 1;

/** Delay before a scheduled write reaches the disk. */
const SAVE_DEBOUNCE_MS = 200;

/** A process as it is written to `hub.json`. Snake case is the on-disk format. */
export interface StoredProcess {
    pid: number;
    pid_start_time: string | null;
    started_at: string;
    command: string;
    args: string[];
    cwd: string;
    pid_file: string | null;
    log_path: string | null;
    state: string;
}

/** One session as it is written to `hub.json`. */
export interface StoredSession {
    id: string;
    token: string;
    /** Written verbatim: a spec the hub has not normalized yet is still stored. */
    spec: object;
    created_at: string;
    process: StoredProcess | null;
}

/** The document this module writes. */
export interface StateDocument {
    version: number;
    saved_at: string;
    sessions: StoredSession[];
}

/**
 * The document as it is read back.
 *
 * `sessions` is `unknown[]` on purpose. The file may have been edited by hand or
 * written by an older hub, so trusting its shape at the type level would move a
 * runtime check into a comment. The caller narrows each entry.
 */
export interface LoadedState {
    version: number;
    sessions: unknown[];
}

/** The slice of a session this module persists. */
export interface PersistableProcess {
    pid: number | null;
    pidStartTime: string | null;
    startedAt: string;
    command: string;
    args: string[];
    cwd: string;
    pidFile?: string | null | undefined;
    logPath?: string | null | undefined;
    state: string;
}

/** The slice of a session record this module persists. */
export interface PersistableSession {
    id: string;
    token: string;
    /** Whatever the session was launched with; `object` because both a raw spec
     * and a normalized one reach here. */
    spec?: object | undefined;
    createdAt: string;
    process?: PersistableProcess | null | undefined;
}

/** Everything `HubState` needs. */
export interface HubStateOptions {
    config: { dataDir: string };
    log: Logger;
}

/** Durable hub state: sessions, tokens, and process identity. */
export class HubState {
    readonly config: { dataDir: string };
    readonly log: Logger;
    readonly path: string;
    timer: NodeJS.Timeout | null;
    pending: PersistableSession[] | null;

    constructor({ config, log }: HubStateOptions) {
        this.config = config;
        this.log = log;
        this.path = join(config.dataDir, 'hub.json');
        this.timer = null;
        this.pending = null;
    }

    /**
     * Read stored state.
     *
     * A missing file is normal (first run). A corrupt file is reported and
     * treated as empty rather than preventing startup: the worker snapshots are
     * the authoritative conversation state, and refusing to boot would make a
     * damaged bookkeeping file unrecoverable.
     */
    load(): LoadedState {
        if (!existsSync(this.path)) return { version: STATE_VERSION, sessions: [] };
        try {
            const parsed: unknown = JSON.parse(readFileSync(this.path, 'utf8'));
            if (typeof parsed !== 'object' || parsed === null
                || !Array.isArray((parsed as { sessions?: unknown }).sessions)) {
                throw new Error('state file has no session list');
            }
            const document = parsed as { version?: unknown; sessions: unknown[] };
            return {
                version: typeof document.version === 'number' ? document.version : STATE_VERSION,
                sessions: document.sessions,
            };
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            this.log.error(`could not read ${this.path}: ${message}; starting empty`);
            return { version: STATE_VERSION, sessions: [] };
        }
    }

    /** Build the document to persist for the given sessions. */
    document(sessions: PersistableSession[]): StateDocument {
        return {
            version: STATE_VERSION,
            saved_at: new Date().toISOString(),
            sessions: sessions.map((session) => ({
                id: session.id,
                token: session.token,
                spec: session.spec ?? {},
                created_at: session.createdAt,
                process: session.process && session.process.pid
                    ? {
                        pid: session.process.pid,
                        pid_start_time: session.process.pidStartTime,
                        started_at: session.process.startedAt,
                        command: session.process.command,
                        args: session.process.args,
                        cwd: session.process.cwd,
                        pid_file: session.process.pidFile ?? null,
                        log_path: session.process.logPath ?? null,
                        state: session.process.state,
                    }
                    : null,
            })),
        };
    }

    /** Schedule a debounced save. Repeated calls collapse into one write. */
    schedule(sessions: PersistableSession[]): void {
        this.pending = sessions;
        if (this.timer) return;
        this.timer = setTimeout(() => {
            this.timer = null;
            const snapshot = this.pending;
            this.pending = null;
            if (snapshot) this.save(snapshot);
        }, SAVE_DEBOUNCE_MS);
        this.timer.unref?.();
    }

    /** Write state immediately, atomically. */
    save(sessions: PersistableSession[]): boolean {
        try {
            mkdirSync(dirname(this.path), { recursive: true });
            const temporary = `${this.path}.tmp`;
            writeFileSync(temporary, `${JSON.stringify(this.document(sessions), null, 2)}\n`);
            renameSync(temporary, this.path);
            return true;
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            this.log.error(`could not save ${this.path}: ${message}`);
            return false;
        }
    }

    /** Flush a pending save and stop the timer. */
    flush(sessions?: PersistableSession[]): boolean {
        if (this.timer) {
            clearTimeout(this.timer);
            this.timer = null;
        }
        const snapshot = sessions ?? this.pending;
        this.pending = null;
        if (snapshot) return this.save(snapshot);
        return false;
    }
}
