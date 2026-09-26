/**
 * @file per-session event transcript.
 *
 * The worker has no replay cursor and its sequence numbers do not acknowledge
 * delivery, so the hub keeps its own bounded, monotonic view of what it saw:
 * `hub_sequence` counts retained conversation envelopes in this hub process,
 * which is what a reconnecting panel resumes from. Transient history-query
 * replies are forwarded live without consuming this sequence or its budget.
 *
 * The same stream is appended to a JSONL file. That file is an operator
 * artifact — the authoritative conversation lives in the worker's own
 * `state.json` — so a hub restart starts an empty in-memory transcript instead
 * of replaying the file.
 */
import { createWriteStream, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import type { WriteStream } from 'node:fs';
import { RingBuffer } from '../util/ring.ts';
import type { Logger } from '../log.ts';

/**
 * An envelope as far as the transcript cares.
 *
 * The transcript reads `bytes` and writes `hub_sequence`, so it is typed by what
 * it touches rather than by the full envelope shape — what a panel sees in an
 * envelope is `shared/protocol.ts`'s business, not this module's.
 */
export interface TranscriptEnvelope {
    bytes?: number;
    hub_sequence?: number;
    [field: string]: unknown;
}

/** Options for one session's transcript. */
export interface SessionTranscriptOptions {
    sessionId: string;
    /** Retained envelopes. */
    limit: number;
    /** Retained wire bytes; zero disables the byte budget. */
    byteLimit?: number;
    /** JSONL path; omit to keep memory only. */
    filePath?: string | null;
}

/** One session's bounded event history plus its append-only log. */
export class SessionTranscript {
    readonly sessionId: string;
    readonly buffer: RingBuffer<TranscriptEnvelope>;
    readonly filePath: string | null;
    stream: WriteStream | null;
    sequence: number;
    written: number;

    constructor({ sessionId, limit, byteLimit = 0, filePath }: SessionTranscriptOptions) {
        this.sessionId = sessionId;
        this.buffer = new RingBuffer<TranscriptEnvelope>({
            limit,
            byteLimit,
            sizeOf: (envelope) => envelope.bytes ?? 0,
        });
        this.filePath = filePath ?? null;
        this.stream = null;
        this.sequence = 0;
        this.written = 0;
    }

    /**
     * Record one envelope, assigning its hub sequence number.
     *
     * @returns the same envelope, with `hub_sequence` set.
     */
    append<T extends TranscriptEnvelope>(envelope: T): T {
        this.sequence += 1;
        envelope.hub_sequence = this.sequence;
        this.buffer.push(envelope);
        this.write(envelope);
        return envelope;
    }

    /** Append one line to the JSONL log, creating the file on first use. */
    write(envelope: TranscriptEnvelope): void {
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
     *
     * @param since exclusive lower bound; 0 returns everything held.
     * @param limit maximum envelopes returned; 0 returns all of them.
     */
    since(since = 0, limit = 0): TranscriptEnvelope[] {
        const items = this.buffer.toArray().filter((item) => (item.hub_sequence ?? 0) > since);
        return limit > 0 ? items.slice(-limit) : items;
    }

    /** Everything currently retained. */
    toArray(): TranscriptEnvelope[] {
        return this.buffer.toArray();
    }

    /** Number of retained envelopes. */
    get size(): number {
        return this.buffer.size;
    }

    /** Envelopes dropped because the ring is full. */
    get dropped(): number {
        return this.buffer.dropped;
    }

    /** Close the JSONL stream. */
    close(): void {
        try {
            this.stream?.end();
        } catch { /* already closed */ }
        this.stream = null;
    }
}

/** The slice of hub configuration the transcript store reads. */
export interface TranscriptStoreConfig {
    dataDir: string;
    limits: { transcriptEvents: number; transcriptBytes: number };
}

/** Everything `TranscriptStore` needs. */
export interface TranscriptStoreOptions {
    config: TranscriptStoreConfig;
    log: Logger;
}

/** Transcript store for every session the hub knows. */
export class TranscriptStore {
    readonly config: TranscriptStoreConfig;
    readonly log: Logger;
    readonly transcripts: Map<string, SessionTranscript>;

    constructor({ config, log }: TranscriptStoreOptions) {
        this.config = config;
        this.log = log;
        this.transcripts = new Map();
    }

    /** Transcript for a session, created on first use. */
    get(sessionId: string): SessionTranscript {
        let transcript = this.transcripts.get(sessionId);
        if (!transcript) {
            transcript = new SessionTranscript({
                sessionId,
                limit: this.config.limits.transcriptEvents,
                byteLimit: this.config.limits.transcriptBytes,
                filePath: join(this.config.dataDir, 'events', `${sessionId}.jsonl`),
            });
            this.transcripts.set(sessionId, transcript);
        }
        return transcript;
    }

    /** Drop a session's transcript and close its log. */
    remove(sessionId: string): void {
        this.transcripts.get(sessionId)?.close();
        this.transcripts.delete(sessionId);
    }

    /** Close every transcript. */
    close(): void {
        for (const transcript of this.transcripts.values()) transcript.close();
        this.transcripts.clear();
    }
}
