/**
 * @file reading the structure out of a tool result's prose.
 *
 * A tool result arrives as one text blob. For the intrinsic process tools that
 * blob is not arbitrary: `textformat::metadata_field` writes `[[name]]: value`
 * lines and `ToolResult::block` writes `name (N bytes):` sections followed by
 * their body — the captured output above already shows both.
 *
 * Two things are deliberately true of this module:
 *
 * 1. **It is a display heuristic, and the producer says so.** `document.hpp`
 *    states that the markers "are not a machine protocol or a security
 *    boundary" — they exist to distinguish metadata visually. Using them to
 *    distinguish it visually is exactly their purpose; deriving anything
 *    authoritative from them would not be, so nothing here decides success,
 *    exit status, or safety. The raw text is always still reachable.
 * 2. **It gives up rather than guesses.** Output from a tool that does not use
 *    this format falls back to plain text, and a partially recognised document
 *    keeps the unrecognised remainder verbatim instead of dropping it.
 *
 * The alternative — dumping `stdout (12 bytes):` into a `<pre>` and calling it
 * done — is what the old panel did, and it is why a tool result read as a wall
 * of punctuation rather than as "the command printed this".
 */

/** One `[[name]]: value` line. */
export interface OutputField {
    readonly name: string;
    readonly value: string;
}

/** One `name (N bytes):` section. */
export interface OutputBlock {
    readonly name: string;
    readonly text: string;
    /** The size the producer reported, or null when it reported none. */
    readonly bytes: number | null;
    /** The producer truncated the body; what is here is the first `bytes`. */
    readonly truncated: boolean;
    readonly empty: boolean;
}

/** A recognised tool-result document. */
export interface OutputDocument {
    readonly fields: readonly OutputField[];
    readonly blocks: readonly OutputBlock[];
    /** Lines that belong to neither, kept verbatim rather than dropped. */
    readonly rest: readonly string[];
    /**
     * A field the producer reported as milliseconds of process runtime.
     *
     * Read only to *label* it: it is what the worker measured, not a duration
     * this panel computed, and it is absent for tools that do not report one.
     */
    readonly reportedMs: number | null;
}

/** The result of looking at a tool result. */
export type ToolOutput =
    | { readonly kind: 'document'; readonly document: OutputDocument }
    /** Not in the block format: show it as it came. */
    | { readonly kind: 'text'; readonly text: string };

/** `[[name]]: value` */
const FIELD = /^\[\[([^\]]+)\]\]: ?(.*)$/;

/**
 * `name (12 bytes):`, `name (truncated, first 12 bytes):`, `name: (empty)`,
 * `name: (empty, truncated)` — the four forms `ToolResult::block` writes.
 */
const BLOCK = /^(\S.*?) \((?:truncated, first )?(\d+) bytes\):$/;
const EMPTY_BLOCK = /^(\S.*?): \((empty|empty, truncated)\)$/;

/** The field the process tools report their own runtime in. */
const RUNTIME_FIELD = 'running_milliseconds';

/** What a candidate block header says, or null. */
function readHeader(line: string): { name: string; bytes: number | null; truncated: boolean; empty: boolean } | null {
    const measured = BLOCK.exec(line);
    if (measured) {
        return {
            name: measured[1]!,
            bytes: Number.parseInt(measured[2]!, 10),
            truncated: line.includes('(truncated,'),
            empty: false,
        };
    }
    const empty = EMPTY_BLOCK.exec(line);
    if (empty) {
        return {
            name: empty[1]!,
            bytes: null,
            truncated: empty[2] === 'empty, truncated',
            empty: true,
        };
    }
    return null;
}

/**
 * Parse a tool result's text.
 *
 * Recognises a leading run of `[[name]]: value` fields followed by zero or more
 * `name (N bytes):` blocks. Anything that does not fit is returned as `rest`
 * (or, when nothing fits at all, as plain text), so an unfamiliar tool's output
 * is shown rather than swallowed.
 */
export function parseToolOutput(raw: string): ToolOutput {
    if (!raw) return { kind: 'text', text: raw };
    const lines = raw.split('\n');
    const fields: OutputField[] = [];
    const blocks: OutputBlock[] = [];
    const rest: string[] = [];
    let index = 0;

    while (index < lines.length) {
        const match = FIELD.exec(lines[index]!);
        if (!match) break;
        fields.push({ name: match[1]!, value: match[2]! });
        index += 1;
    }

    // The producer separates the metadata from the blocks, and one block from
    // the next, with a single blank line. A blank line inside a body is kept:
    // only a blank line whose successor is a header ends the body.
    const isBoundary = (at: number): boolean => {
        if (lines[at] !== '') return false;
        const next = lines[at + 1];
        return next !== undefined && (readHeader(next) !== null || next === '---');
    };

    while (index < lines.length) {
        if (lines[index] === '') {
            index += 1;
            continue;
        }
        if (lines[index] === '---') {
            // Records are joined with this separator; it carries no content.
            index += 1;
            continue;
        }
        const header = readHeader(lines[index]!);
        if (!header) {
            rest.push(...lines.slice(index));
            break;
        }
        index += 1;
        const body: string[] = [];
        while (index < lines.length && !isBoundary(index)) {
            body.push(lines[index]!);
            index += 1;
        }
        // The producer terminates every non-empty body with a newline, which
        // the split turned into a trailing empty element.
        while (body.length > 0 && body[body.length - 1] === '') body.pop();
        blocks.push({ ...header, text: body.join('\n') });
    }

    if (fields.length === 0 && blocks.length === 0) {
        return { kind: 'text', text: raw };
    }

    const runtime = fields.find((field) => field.name === RUNTIME_FIELD);
    const reported = runtime ? Number.parseInt(runtime.value, 10) : Number.NaN;
    return {
        kind: 'document',
        document: {
            fields,
            blocks,
            rest,
            reportedMs: Number.isFinite(reported) ? reported : null,
        },
    };
}
