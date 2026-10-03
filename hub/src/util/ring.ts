/**
 * @file bounded in-memory buffers.
 *
 * Worker stdout/stderr and the transcript are both unbounded streams owned by
 * another process, so the hub keeps a fixed-size tail of each in memory and
 * optionally writes a best-effort copy to disk.
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
 * Each independent pipe needs its own instance. The decoded UTF-8 prefix of
 * one line is limited to 64 KiB by default; excess bytes are counted and
 * discarded until LF or EOF, then reported in a suffix on the retained line.
 * This bounds partial lines without turning newline-free output into an
 * unlimited number of synthetic lines. CRLF is normalized to LF and its CR
 * does not consume the content budget, even across chunk boundaries.
 */
export class LineSplitter {
    readonly onLine: (line: string) => void;
    pending: string;
    decoder: TextDecoder;
    readonly maxLineBytes: number;
    pendingBytes = 0;
    /** Total decoded UTF-8 bytes omitted, including the current partial line. */
    truncatedBytes = 0;
    private omittedBytes = 0;
    /** Hold one trailing CR until it can be distinguished from a CRLF ending. */
    private pendingCarriageReturn = false;

    constructor(onLine: (line: string) => void, maxLineBytes = 64 * 1024) {
        if (!Number.isSafeInteger(maxLineBytes) || maxLineBytes <= 0) {
            throw new RangeError('maxLineBytes must be a positive safe integer');
        }
        this.onLine = onLine;
        this.maxLineBytes = maxLineBytes;
        this.pending = '';
        this.decoder = new TextDecoder('utf8');
    }

    /** Feed one chunk of bytes, or text that is already decoded. */
    push(chunk: string | Uint8Array): void {
        const text = typeof chunk === 'string'
            ? chunk
            : this.decoder.decode(chunk, { stream: true });
        this.consume(text);
    }

    /** Consume decoded text without appending an unbounded chunk to pending. */
    private consume(text: string): void {
        let start = 0;
        while (start < text.length) {
            const newline = text.indexOf('\n', start);
            const end = newline === -1 ? text.length : newline;
            const segment = text.slice(start, end);
            if (this.pendingCarriageReturn && segment.length > 0) {
                // More content followed the previous CR, so it was not a terminator.
                this.appendContent('\r');
            }
            this.pendingCarriageReturn = segment.endsWith('\r');
            this.appendContent(this.pendingCarriageReturn ? segment.slice(0, -1) : segment);
            if (newline === -1) return;
            this.emit();
            start = newline + 1;
        }
    }

    /** Retain a bounded UTF-8 prefix, counting only omitted line content. */
    private appendContent(segment: string): void {
        const bytes = Buffer.byteLength(segment, 'utf8');
        let retained = 0;
        if (this.omittedBytes === 0) {
            const available = this.maxLineBytes - this.pendingBytes;
            if (bytes <= available) {
                this.pending += segment;
                retained = bytes;
            } else {
                const encoded = Buffer.from(segment, 'utf8');
                let cut = available;
                // Do not retain a partial UTF-8 code point at the boundary.
                while (cut > 0 && (encoded[cut]! & 0xc0) === 0x80) cut -= 1;
                this.pending += encoded.subarray(0, cut).toString('utf8');
                retained = cut;
            }
            this.pendingBytes += retained;
        }
        this.omittedBytes += bytes - retained;
        this.truncatedBytes += bytes - retained;
    }

    /** Emit one line and reset its prefix and omission counters. */
    private emit(): void {
        const line = this.omittedBytes > 0
            ? `${this.pending} [hub: truncated ${this.omittedBytes} UTF-8 bytes]`
            : this.pending;
        this.pending = '';
        this.pendingBytes = 0;
        this.omittedBytes = 0;
        this.pendingCarriageReturn = false;
        this.onLine(line);
    }

    /** Flush the decoder at EOF, including an incomplete final UTF-8 sequence. */
    flush(): void {
        this.consume(this.decoder.decode());
        // Preserve the existing normalization of a trailing CR at EOF too.
        if (this.pending.length > 0 || this.omittedBytes > 0 || this.pendingCarriageReturn) {
            this.emit();
        }
    }
}
