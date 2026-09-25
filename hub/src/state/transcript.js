/**
 * @file per-session event transcript.
 *
 * The worker has no replay cursor and its sequence numbers do not acknowledge
 * delivery, so the hub keeps its own bounded, monotonic view of what it saw:
 * `hub_sequence` counts envelopes received in this hub process, which is what a
 * reconnecting panel resumes from.
 *
 * The same stream is appended to a JSONL file. That file is an operator
 * artifact — the authoritative conversation lives in the worker's own
 * `state.json` — so a hub restart starts an empty in-memory transcript instead
 * of replaying the file.
 */
import { createWriteStream, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { RingBuffer } from '../util/ring.js';

/** One session's bounded event history plus its append-only log. */
export class SessionTranscript {
    /**
     * @param {object} options
     * @param {string} options.sessionId
     * @param {number} options.limit retained envelopes.
     * @param {string} [options.filePath] JSONL path; omit to keep memory only.
     */
    constructor({ sessionId, limit, filePath }) {
        this.sessionId = sessionId;
        this.buffer = new RingBuffer({ limit });
        this.filePath = filePath ?? null;
        this.stream = null;
        this.sequence = 0;
        this.written = 0;
    }

    /**
     * Record one envelope, assigning its hub sequence number.
     * @returns {object} the same envelope, with `hub_sequence` set.
     */
    append(envelope) {
        this.sequence += 1;
        envelope.hub_sequence = this.sequence;
        this.buffer.push(envelope);
        this.write(envelope);
        return envelope;
    }

    /** Append one line to the JSONL log, creating the file on first use. */
    write(envelope) {
        if (!this.filePath) return;
        try {
            if (!this.stream) {
                mkdirSync(dirname(this.filePath), { recursive: true });
                this.stream = createWriteStream(this.filePath, { flags: 'a' });
                this.stream.on('error', () => { this.stream = null; });
            }
            this.stream.write(`${JSON.stringify(envelope)}\n`);
            this.written += 1;
        } catch {
            // Losing the optional log must not disturb the live stream.
            this.stream = null;
        }
    }

    /**
     * Envelopes after a hub sequence number.
     * @param {number} since exclusive lower bound; 0 returns everything held.
     * @param {number} [limit] maximum envelopes returned.
     */
    since(since = 0, limit = 0) {
        const items = this.buffer.toArray().filter((item) => item.hub_sequence > since);
        return limit > 0 ? items.slice(-limit) : items;
    }

    /** Everything currently retained. */
    toArray() {
        return this.buffer.toArray();
    }

    /** Number of retained envelopes. */
    get size() {
        return this.buffer.size;
    }

    /** Envelopes dropped because the ring is full. */
    get dropped() {
        return this.buffer.dropped;
    }

    /** Close the JSONL stream. */
    close() {
        try {
            this.stream?.end();
        } catch { /* already closed */ }
        this.stream = null;
    }
}

/** Transcript store for every session the hub knows. */
export class TranscriptStore {
    /**
     * @param {object} options
     * @param {object} options.config hub configuration.
     * @param {object} options.log hub logger.
     */
    constructor({ config, log }) {
        this.config = config;
        this.log = log;
        this.transcripts = new Map();
    }

    /** Transcript for a session, created on first use. */
    get(sessionId) {
        let transcript = this.transcripts.get(sessionId);
        if (!transcript) {
            transcript = new SessionTranscript({
                sessionId,
                limit: this.config.limits.transcriptEvents,
                filePath: join(this.config.dataDir, 'events', `${sessionId}.jsonl`),
            });
            this.transcripts.set(sessionId, transcript);
        }
        return transcript;
    }

    /** Drop a session's transcript and close its log. */
    remove(sessionId) {
        this.transcripts.get(sessionId)?.close();
        this.transcripts.delete(sessionId);
    }

    /** Close every transcript. */
    close() {
        for (const transcript of this.transcripts.values()) transcript.close();
        this.transcripts.clear();
    }
}
