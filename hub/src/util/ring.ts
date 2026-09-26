/**
 * @file bounded in-memory buffers.
 *
 * Worker stdout/stderr and the transcript are both unbounded streams owned by
 * another process, so the hub keeps a fixed-size tail of each in memory and
 * writes the full stream somewhere else when it needs to keep it.
 */

/** Options accepted by `RingBuffer`. */
export interface RingBufferOptions<T> {
    /** Maximum entries retained. */
    limit: number;
    /** Maximum total size retained; zero disables the byte budget. */
    byteLimit?: number;
    /**
     * Size of one entry. Defaults to string length, which is what a log ring
     * needs; a ring of envelopes passes its own measure.
     */
    sizeOf?: (item: T) => number;
}

/**
 * A ring of the most recent `limit` entries, with an optional byte budget for
 * entry text.
 */
export class RingBuffer<T = string> {
    readonly limit: number;
    readonly byteLimit: number;
    readonly sizeOf: (item: T) => number;
    items: T[];
    bytes: number;
    dropped: number;

    constructor({ limit, byteLimit = 0, sizeOf }: RingBufferOptions<T>) {
        this.limit = limit;
        this.byteLimit = byteLimit;
        this.sizeOf = sizeOf
            ?? ((item) => (typeof item === 'string' ? item.length : 0) as number);
        this.items = [];
        this.bytes = 0;
        this.dropped = 0;
    }

    /** Append one entry, evicting the oldest as needed. */
    push(item: T): void {
        this.items.push(item);
        this.bytes += this.sizeOf(item);
        while (this.items.length > this.limit
            || (this.byteLimit > 0 && this.bytes > this.byteLimit && this.items.length > 1)) {
            // The loop conditions both imply a nonempty ring, which is what makes
            // this shift defined.
            const removed = this.items.shift() as T;
            this.bytes -= this.sizeOf(removed);
            this.dropped += 1;
        }
    }

    /** Entries in insertion order. */
    toArray(): T[] {
        return [...this.items];
    }

    /** Number of retained entries. */
    get size(): number {
        return this.items.length;
    }

    /** Drop every retained entry. */
    clear(): void {
        this.items = [];
        this.bytes = 0;
    }
}

/**
 * Incremental UTF-8 line splitter.
 *
 * Child output arrives in arbitrary chunks; a line must not be split just
 * because a read boundary fell in the middle of it.
 */
export class LineSplitter {
    readonly onLine: (line: string) => void;
    pending: string;
    decoder: TextDecoder;

    constructor(onLine: (line: string) => void) {
        this.onLine = onLine;
        this.pending = '';
        this.decoder = new TextDecoder('utf8');
    }

    /** Feed one chunk of bytes, or text that is already decoded. */
    push(chunk: string | Uint8Array): void {
        this.pending += typeof chunk === 'string'
            ? chunk
            : this.decoder.decode(chunk, { stream: true });
        for (;;) {
            const index = this.pending.indexOf('\n');
            if (index === -1) break;
            const line = this.pending.slice(0, index).replace(/\r$/, '');
            this.pending = this.pending.slice(index + 1);
            this.onLine(line);
        }
    }

    /** Emit whatever is left, without a trailing newline. */
    flush(): void {
        if (this.pending.length === 0) return;
        const line = this.pending.replace(/\r$/, '');
        this.pending = '';
        this.onLine(line);
    }
}
