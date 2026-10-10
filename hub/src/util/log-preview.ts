/** Minimum room for a UTF-8 prefix plus the complete truncation annotation. */
export const MIN_LOG_RING_BYTES = 64;

/** Render one decoded line; the count refers to content, never to this suffix. */
export function formatLogLine(prefix: string, omittedBytes: number): string {
    return omittedBytes > 0
        ? `${prefix} [hub: truncated ${omittedBytes} UTF-8 bytes]`
        : prefix;
}

export interface LogPreview {
    /** Complete ring entry, including its omission annotation. */
    text: string;
    /** Additional content removed for the ring, excluding prior line truncation. */
    truncatedBytes: number;
}

/**
 * Fit a split line into the log ring's UTF-8 budget before admission. Reserve
 * space for the largest possible omission count, then clip at a code-point
 * boundary. Existing splitter omissions are folded into the one annotation;
 * only newly removed content is returned for additional accounting. The disk
 * sink can independently use formatLogLine() with the original split prefix.
 */
export function logPreview(prefix: string, omittedBytes: number, maxBytes: number): LogPreview {
    if (!Number.isSafeInteger(maxBytes) || maxBytes < MIN_LOG_RING_BYTES) {
        throw new RangeError(`log preview budget must be a safe integer >= ${MIN_LOG_RING_BYTES}`);
    }
    const full = formatLogLine(prefix, omittedBytes);
    if (Buffer.byteLength(full, 'utf8') <= maxBytes) {
        return { text: full, truncatedBytes: 0 };
    }

    const bytes = Buffer.from(prefix, 'utf8');
    const suffix = formatLogLine('', omittedBytes + bytes.length);
    let end = Math.max(0, maxBytes - Buffer.byteLength(suffix, 'utf8'));
    end = Math.min(end, bytes.length);
    while (end > 0 && end < bytes.length && (bytes[end]! & 0xc0) === 0x80) end -= 1;
    const truncatedBytes = bytes.length - end;
    return {
        text: formatLogLine(bytes.subarray(0, end).toString('utf8'), omittedBytes + truncatedBytes),
        truncatedBytes,
    };
}
