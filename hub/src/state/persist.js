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

/** Bumped when the stored shape changes incompatibly. */
export const STATE_VERSION = 1;

/** Delay before a scheduled write reaches the disk. */
const SAVE_DEBOUNCE_MS = 200;

/** Durable hub state: sessions, tokens, and process identity. */
export class HubState {
    /**
     * @param {object} options
     * @param {object} options.config hub configuration.
     * @param {object} options.log hub logger.
     */
    constructor({ config, log }) {
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
     *
     * @returns {{version: number, sessions: object[]}}
     */
    load() {
        if (!existsSync(this.path)) return { version: STATE_VERSION, sessions: [] };
        try {
            const parsed = JSON.parse(readFileSync(this.path, 'utf8'));
            if (typeof parsed !== 'object' || parsed === null || !Array.isArray(parsed.sessions)) {
                throw new Error('state file has no session list');
            }
            return { version: parsed.version ?? STATE_VERSION, sessions: parsed.sessions };
        } catch (error) {
            this.log.error(`could not read ${this.path}: ${error.message}; starting empty`);
            return { version: STATE_VERSION, sessions: [] };
        }
    }

    /** Build the document to persist for the given sessions. */
    document(sessions) {
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
    schedule(sessions) {
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
    save(sessions) {
        try {
            mkdirSync(dirname(this.path), { recursive: true });
            const temporary = `${this.path}.tmp`;
            writeFileSync(temporary, `${JSON.stringify(this.document(sessions), null, 2)}\n`);
            renameSync(temporary, this.path);
            return true;
        } catch (error) {
            this.log.error(`could not save ${this.path}: ${error.message}`);
            return false;
        }
    }

    /** Flush a pending save and stop the timer. */
    flush(sessions) {
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
