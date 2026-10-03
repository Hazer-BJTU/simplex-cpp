/**
 * @file bounded, best-effort disk logging without a secondary application queue.
 *
 * A false Writable.write() result admits that record, then closes admission
 * until drain. Later records are dropped rather than delaying live routing.
 * The UTF-8 byte budget also rejects a single oversized record. Errors disable
 * this writer permanently; it never repeatedly reopens a failing log file.
 */
import type { Writable } from 'node:stream';

/** Maximum queued UTF-8 bytes per optional log file. */
export const LOG_WRITE_MAX_BYTES = 1024 * 1024;
/** Grace period for best-effort log flushing after end(). */
export const LOG_WRITE_CLOSE_MS = 1000;

/** A contiguous group of records rejected by admission. */
export interface LogOmission {
    records: number;
    bytes: number;
}

/** Dependencies and policy for one file; open is called lazily, at most once. */
export interface BoundedWriterOptions {
    open: () => Writable;
    /** Render a bounded omission marker, including its newline. */
    omission: (loss: LogOmission) => string;
    /** Called once on the first dropped record. Must not enqueue log data here. */
    onDrop?: (() => void) | undefined;
    /** Called once on failure; the optional log must not end its owner. */
    onError?: ((error: Error) => void) | undefined;
    maxBytes?: number;
    closeTimeoutMs?: number;
}

/**
 * One bounded Writable owner. write() reports queue admission, not delivery or
 * durability. Drops count rejected records, not accepted writes lost to a later
 * disk error. abandonedBytes records the pending byte count at such a failure.
 * end() rejects new records, tries to append a final omission marker and finish,
 * then destroys a stalled sink after a finite deadline. It never blocks callers.
 */
export class BoundedWriter {
    readonly maxBytes: number;
    readonly closeTimeoutMs: number;
    droppedRecords = 0;
    droppedBytes = 0;
    abandonedBytes = 0;
    failed = false;
    private readonly options: BoundedWriterOptions;
    private stream: Writable | null = null;
    private blocked = false;
    private closing = false;
    private ending = false;
    private finished = false;
    private pendingLoss: LogOmission = { records: 0, bytes: 0 };
    private closeTimer: NodeJS.Timeout | null = null;

    constructor(options: BoundedWriterOptions) {
        this.options = options;
        this.maxBytes = options.maxBytes ?? LOG_WRITE_MAX_BYTES;
        this.closeTimeoutMs = options.closeTimeoutMs ?? LOG_WRITE_CLOSE_MS;
        if (!Number.isSafeInteger(this.maxBytes) || this.maxBytes < 256) {
            throw new RangeError('maxBytes must be a safe integer of at least 256');
        }
        if (!Number.isSafeInteger(this.closeTimeoutMs) || this.closeTimeoutMs <= 0) {
            throw new RangeError('closeTimeoutMs must be a positive safe integer');
        }
    }

    /** Includes the in-flight write and the Writable's own queued data. */
    get pendingBytes(): number {
        return this.stream?.writableLength ?? 0;
    }

    /** Admit one whole record, or count its omission without holding its text. */
    write(record: string): boolean {
        if (!this.closing && !this.failed && this.flushOmission() && this.admit(record)) {
            return true;
        }
        const bytes = Buffer.byteLength(record, 'utf8');
        this.droppedRecords += 1;
        this.droppedBytes += bytes;
        this.pendingLoss.records += 1;
        this.pendingLoss.bytes += bytes;
        if (this.droppedRecords === 1) {
            try { this.options.onDrop?.(); } catch { /* diagnostics are optional */ }
        }
        return false;
    }

    /** Write without adding the rejected text to any application-side queue. */
    private admit(record: string): boolean {
        if (this.failed || this.blocked) return false;
        const bytes = Buffer.byteLength(record, 'utf8');
        if (this.pendingBytes + bytes > this.maxBytes) return false;
        try {
            if (!this.stream) {
                this.stream = this.options.open();
                this.stream.on('error', (error: Error) => this.fail(error));
                this.stream.on('drain', () => {
                    this.blocked = false;
                    this.flushOmission();
                    if (this.closing) this.finish();
                });
                this.stream.once('finish', () => {
                    this.finished = true;
                    this.clearTimer();
                });
                this.stream.once('close', () => {
                    if (!this.finished && !this.failed) {
                        this.fail(new Error('log sink closed before finishing'));
                    }
                    this.clearTimer();
                });
            }
            if (this.stream.destroyed || this.stream.writableEnded) {
                this.fail(new Error('log sink is not writable'));
                return false;
            }
            // A Buffer keeps writableLength in bytes even if a supplied sink
            // was constructed with decodeStrings: false.
            this.blocked = !this.stream.write(Buffer.from(record, 'utf8'));
            return true;
        } catch (error) {
            this.fail(error instanceof Error ? error : new Error(String(error)));
            return false;
        }
    }

    /** Preserve omission order ahead of the next accepted record. */
    private flushOmission(): boolean {
        if (this.pendingLoss.records === 0) return true;
        if (this.failed || this.blocked || this.ending) return false;
        try {
            if (!this.admit(this.options.omission(this.pendingLoss))) return false;
            this.pendingLoss = { records: 0, bytes: 0 };
            return true;
        } catch (error) {
            this.fail(error instanceof Error ? error : new Error(String(error)));
            return false;
        }
    }

    /** Disable admission once and release the failed sink. */
    private fail(error: Error): void {
        if (this.failed) return;
        this.failed = true;
        this.abandonedBytes = this.pendingBytes;
        this.clearTimer();
        this.stream?.destroy();
        try { this.options.onError?.(error); } catch { /* diagnostics are optional */ }
    }

    /** Start a finite best-effort flush; repeated calls have no effect. */
    end(): void {
        if (this.closing) return;
        this.closing = true;
        if (this.failed) return;
        if (!this.blocked || this.pendingLoss.records === 0) {
            this.flushOmission();
            this.finish();
        }
        if (this.stream && !this.finished && !this.failed) {
            this.closeTimer = setTimeout(() => {
                this.fail(new Error('log sink did not finish before its close deadline'));
            }, this.closeTimeoutMs);
            this.closeTimer.unref();
        }
    }

    /** Tell the sink there are no further records, after any omission marker. */
    private finish(): void {
        if (this.ending || this.failed) return;
        this.ending = true;
        try { this.stream?.end(); }
        catch (error) {
            this.fail(error instanceof Error ? error : new Error(String(error)));
        }
    }

    private clearTimer(): void {
        if (this.closeTimer) clearTimeout(this.closeTimer);
        this.closeTimer = null;
    }
}
