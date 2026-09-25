/**
 * @file bounded in-memory buffers.
 *
 * Worker stdout/stderr and the transcript are both unbounded streams owned by
 * another process, so the hub keeps a fixed-size tail of each in memory and
 * writes the full stream somewhere else when it needs to keep it.
 */

/**
 * A ring of the most recent `limit` entries, with an optional byte budget for
 * entry text.
 */
export class RingBuffer {
    /**
     * @param {object} options
     * @param {number} options.limit maximum entries retained.
     * @param {number} [options.byteLimit] maximum total size retained.
     * @param {(item: any) => number} [options.sizeOf] size of one entry;
     *   defaults to string length, which is what a log ring needs.
     */
    constructor({ limit, byteLimit = 0, sizeOf }) {
        this.limit = limit;
        this.byteLimit = byteLimit;
        this.sizeOf = sizeOf ?? ((item) => (typeof item === 'string' ? item.length : 0));
        this.items = [];
        this.bytes = 0;
        this.dropped = 0;
    }

    /** Append one entry, evicting the oldest as needed. */
    push(item) {
        this.items.push(item);
        this.bytes += this.sizeOf(item);
        while (this.items.length > this.limit
            || (this.byteLimit > 0 && this.bytes > this.byteLimit && this.items.length > 1)) {
            const removed = this.items.shift();
            this.bytes -= this.sizeOf(removed);
            this.dropped += 1;
        }
    }

    /** Entries in insertion order. */
    toArray() {
        return [...this.items];
    }

    /** Number of retained entries. */
    get size() {
        return this.items.length;
    }

    /** Drop every retained entry. */
    clear() {
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
    /** @param {(line: string) => void} onLine */
    constructor(onLine) {
        this.onLine = onLine;
        this.pending = '';
        this.decoder = new TextDecoder('utf8');
    }

    /** Feed one chunk of bytes. */
    push(chunk) {
        this.pending += typeof chunk === 'string' ? chunk : this.decoder.decode(chunk, { stream: true });
        for (;;) {
            const index = this.pending.indexOf('\n');
            if (index === -1) break;
            const line = this.pending.slice(0, index).replace(/\r$/, '');
            this.pending = this.pending.slice(index + 1);
            this.onLine(line);
        }
    }

    /** Emit whatever is left, without a trailing newline. */
    flush() {
        if (this.pending.length === 0) return;
        const line = this.pending.replace(/\r$/, '');
        this.pending = '';
        this.onLine(line);
    }
}
