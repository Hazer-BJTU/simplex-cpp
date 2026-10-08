/**
 * @file per-session event transcript.
 *
 * The worker has no replay cursor and its sequence numbers do not acknowledge
 * delivery, so the hub keeps its own bounded, monotonic view of what it saw:
 * `hub_sequence` counts retained conversation envelopes in this hub process,
 * which is what a reconnecting panel resumes from. Transient history-query
 * replies are forwarded live without consuming this sequence or its budget.
 *
 * A best-effort copy is appended to a bounded JSONL writer. That file is an
 * operator artifact — the authoritative conversation lives in the worker's own
 * `state.json` — so a hub restart starts an empty in-memory transcript instead
 * of replaying the file.
 */
import { createWriteStream, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { isSubagentId } from './session-id.ts';
import { sessionDir } from '../launch/config-render.ts';
import { answerSource } from '../../shared/answers.ts';
import { measureEnvelope } from '../protocol/events.ts';
import { RingBuffer } from '../util/ring.ts';
import { BoundedWriter } from '../util/bounded-writer.ts';
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
    /** Optional diagnostics for disk omissions and failures. */
    log?: Logger;
}

/** One session's bounded event history plus its append-only log. */
export class SessionTranscript {
    readonly sessionId: string;
    readonly buffer: RingBuffer<TranscriptEnvelope>;
    readonly filePath: string | null;
    readonly writer: BoundedWriter | null;
    sequence: number;
    /** Envelopes admitted to the file queue, not a durability acknowledgment. */
    written: number;

    constructor({ sessionId, limit, byteLimit = 0, filePath, log }: SessionTranscriptOptions) {
        this.sessionId = sessionId;
        this.buffer = new RingBuffer<TranscriptEnvelope>({
            limit,
            byteLimit,
            sizeOf: (envelope) => envelope.bytes ?? 0,
        });
        this.filePath = filePath ?? null;
        const logPath = this.filePath;
        this.writer = logPath ? new BoundedWriter({
            open: () => {
                mkdirSync(dirname(logPath), { recursive: true });
                return createWriteStream(logPath, { flags: 'a' });
            },
            // This diagnostic is valid JSONL, not a worker event or replay item.
            omission: ({ records, bytes }) => JSON.stringify({
                type: 'hub_log_omission', dropped_records: records, dropped_bytes: bytes,
            }) + '\n',
            onDrop: () => log?.warn(`session ${sessionId}: transcript file records omitted`
                + ' from disk; live replay is unaffected'),
            onError: (error) => log?.warn(
                `session ${sessionId}: transcript file disabled: ${error.message}`),
        }) : null;
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
        // Include forwarding fields and the assigned cursor, not the original ingress text.
        measureEnvelope(envelope);
        if (this.buffer.byteLimit > 0 && (envelope.bytes ?? 0) > this.buffer.byteLimit) {
            // Retain an honest omission with the same correlation/cursor. Never
            // let a single display item poison replay under a smaller operator budget.
            const original = envelope.data as Record<string, unknown> | null;
            (envelope as TranscriptEnvelope).data = { display_omitted: true, original_bytes: envelope.bytes,
                ...(answerSource(original?.answer_source) ? { answer_source: original!.answer_source } : {}) };
            delete envelope.raw;
            measureEnvelope(envelope);
        }
        this.buffer.push(envelope);
        this.write(envelope);
        return envelope;
    }

    /** Append a complete JSONL record if the optional file queue can admit it. */
    write(envelope: TranscriptEnvelope): void {
        if (!this.writer) return;
        try {
            if (this.writer.write(`${JSON.stringify(envelope)}\n`)) this.written += 1;
        } catch {
            // An unserializable envelope must not disturb the live stream.
            this.writer.write('{"type":"hub_log_omission","reason":"serialization failed"}\n');
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

    /** Begin a bounded best-effort flush; later writes cannot reopen the file. */
    close(): void {
        this.writer?.end();
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
        if (isSubagentId(sessionId)) throw new Error('headless workers do not retain an event transcript');
        let transcript = this.transcripts.get(sessionId);
        if (!transcript) {
            transcript = new SessionTranscript({
                sessionId,
                limit: this.config.limits.transcriptEvents,
                byteLimit: this.config.limits.transcriptBytes,
                filePath: join(sessionDir(this.config, sessionId), 'events.jsonl'),
                log: this.log,
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
