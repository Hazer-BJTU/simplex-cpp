/**
 * Approval display copies. The pending call remains the authority for execution.
 * Budgets count encoded JSON bytes, including keys, markers and escaping.
 */
export const APPROVAL_ARGUMENT_BYTES = 64 * 1024;

const OMITTED = { display_omitted: true };
const OMITTED_BYTES = encodedBytes(OMITTED);
const MAX_DEPTH = 12;
const MAX_NODES = 4096;
/** Leave room for a useful prefix instead of filling an array with bare markers. */
const ARRAY_ITEM_BYTES = 128;

function encodedBytes(value: unknown): number {
    return Buffer.byteLength(JSON.stringify(value));
}

/**
 * Keep small values intact; lower the largest allowances first until they fit.
 * Each shortened value has enough space for an explicit omission marker.
 */
function allowances(sizes: number[], maximum: number, minimumBytes = OMITTED_BYTES): number[] | null {
    const minimum = sizes.map(size => Math.min(size, minimumBytes));
    if (minimum.reduce((sum, size) => sum + size, 0) > maximum) return null;
    if (sizes.reduce((sum, size) => sum + size, 0) <= maximum) return sizes;

    let low = 0;
    let high = sizes.reduce((largest, size) => Math.max(largest, size), 0);
    while (low < high) {
        const middle = Math.ceil((low + high) / 2);
        const used = sizes.reduce((sum, size, index) =>
            sum + Math.max(minimum[index]!, Math.min(size, middle)), 0);
        if (used <= maximum) low = middle;
        else high = middle - 1;
    }
    return sizes.map((size, index) => Math.max(minimum[index]!, Math.min(size, low)));
}

/** Preserve a UTF-8 prefix inside a clearly labelled display-only value. */
function stringPreview(value: string, maximum: number): unknown {
    const marker = { display_truncated: true, bytes: Buffer.byteLength(value), preview: '' };
    if (encodedBytes(marker) > maximum) return OMITTED;

    const prefix = (length: number): string => {
        let end = length;
        const previous = value.charCodeAt(end - 1);
        const next = value.charCodeAt(end);
        if (previous >= 0xd800 && previous <= 0xdbff && next >= 0xdc00 && next <= 0xdfff) end -= 1;
        // Keep lone surrogates as JSON escapes rather than replacing them by
        // converting the entire source through a UTF-8 Buffer.
        return value.slice(0, end);
    };
    let low = 0;
    let high = Math.min(value.length, maximum);
    while (low < high) {
        const middle = Math.ceil((low + high) / 2);
        if (encodedBytes({ ...marker, preview: prefix(middle) }) <= maximum) low = middle;
        else high = middle - 1;
    }
    return { ...marker, preview: prefix(low) };
}

/**
 * Only oversized trees are traversed. Keep object keys and small sibling values;
 * a container that cannot fit its keys/markers becomes an explicit omission.
 * Arrays retain a prefix with an omitted-item count when even minimal entries
 * cannot all fit. Depth/node guards bound traversal of hostile argument trees.
 */
function project(value: unknown, maximum: number, depth: number, work: { nodes: number }): unknown {
    if (encodedBytes(value) <= maximum) return value;
    if (work.nodes-- <= 0 || depth >= MAX_DEPTH) return OMITTED;
    if (typeof value === 'string') return stringPreview(value, maximum);

    if (Array.isArray(value)) {
        const sizes = value.map(encodedBytes);
        let count = sizes.length;
        let omittedBytes = 0;
        // Compute the retained prefix in one pass, rather than repeatedly
        // allocating/summing a shortened array for every omitted item.
        let minimumBytes = sizes.reduce((sum, size) => sum + Math.min(size, ARRAY_ITEM_BYTES), 0);
        while (count > 0 && 2 + Math.max(0, count - 1) + minimumBytes + omittedBytes > maximum) {
            count -= 1;
            minimumBytes -= Math.min(sizes[count]!, ARRAY_ITEM_BYTES);
            omittedBytes = 1 + encodedBytes({ display_omitted: true, omitted_items: sizes.length - count });
        }
        if (count === 0) return { display_omitted: true };
        const limits = allowances(sizes.slice(0, count), maximum - 2 - (count - 1) - omittedBytes, ARRAY_ITEM_BYTES)!;
        const result = value.slice(0, count).map((item, index) => project(item, limits[index]!, depth + 1, work));
        if (count < value.length) result.push({ display_omitted: true, omitted_items: value.length - count });
        return result;
    }

    if (value !== null && typeof value === 'object') {
        const entries = Object.entries(value);
        const framing = 2 + Math.max(0, entries.length - 1)
            + entries.reduce((sum, [key]) => sum + encodedBytes(key) + 1, 0);
        const limits = allowances(entries.map(([, item]) => encodedBytes(item)), maximum - framing);
        if (!limits) return OMITTED;
        return Object.fromEntries(entries.map(([key, item], index) =>
            [key, project(item, limits[index]!, depth + 1, work)]));
    }

    return OMITTED;
}

/**
 * Produce at most 64 KiB of encoded argument JSON, preserving exact values when
 * they fit. Marker objects belong only to the display copy, never the tool call.
 * A missing optional arguments field is displayed as an empty object.
 */
export function approvalArgumentPreview(argumentsValue: unknown): {
    value: unknown;
    truncated: boolean;
    originalBytes: number;
} {
    const source = argumentsValue === undefined ? {} : argumentsValue;
    const encoded = JSON.stringify(source);
    const originalBytes = Buffer.byteLength(encoded);
    const truncated = originalBytes > APPROVAL_ARGUMENT_BYTES;
    const preview = truncated
        ? JSON.stringify(project(source, APPROVAL_ARGUMENT_BYTES, 0, { nodes: MAX_NODES })) : encoded;
    // Even unchanged nested values must not alias the authoritative call.
    return { value: JSON.parse(preview), truncated, originalBytes };
}
