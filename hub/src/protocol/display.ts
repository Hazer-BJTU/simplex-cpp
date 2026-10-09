import { answerSource } from '../../shared/answers.ts';
import { toolOmissionCount } from '../../shared/tool-batches.ts';
/** Display copies only: executable inputs and approval authority never use this module. */
const MAX_ENCODED_DATA = 768 * 1024;
const ANSWER_BYTES = 512 * 1024;
const REASONING_BYTES = 4096;
/** Successful compact summaries are complete UTF-8 text, up to the worker's limit. */
const COMPACT_SUMMARY_BYTES = 32 * 1024;
const object = (value: unknown): Record<string, unknown> | null =>
    value !== null && typeof value === 'object' && !Array.isArray(value)
        ? value as Record<string, unknown> : null;

/** A UTF-8 prefix, without replacement characters introduced by the cutoff. */
export function utf8Prefix(text: string, maximum: number): string {
    if (Buffer.byteLength(text) <= maximum) return text;
    const encoded = Buffer.from(text);
    let end = Math.min(maximum, encoded.length);
    while (end > 0 && (encoded[end]! & 0xc0) === 0x80) end -= 1;
    return encoded.subarray(0, end).toString('utf8');
}

/** Bounded diagnostic traversal. Keep field types, mark omission on the containing object. */
export function diagnosticPreview(value: unknown, depth = 0, budget = { nodes: 128 }): unknown {
    if (budget.nodes-- <= 0 || depth > 12) return { display_omitted: true };
    if (typeof value === 'string') return utf8Prefix(value, 1024);
    if (Array.isArray(value)) return value.slice(0, 64).map(item => diagnosticPreview(item, depth + 1, budget));
    const source = object(value);
    if (!source) return value;
    const result: Record<string, unknown> = {};
    let shortened = false;
    for (const [key, item] of Object.entries(source)) {
        if (Object.keys(result).length >= 64 || budget.nodes <= 0 || key.length > 128) { shortened = true; break; }
        result[key] = diagnosticPreview(item, depth + 1, budget);
        if (typeof item === 'string' && result[key] !== item) {
            shortened = true;
            if (key === 'raw') { result.truncated = true; result.bytes = Buffer.byteLength(item); }
        }
    }
    if (shortened) result.display_truncated = true;
    return result;
}

/** Preserve the summary independently of diagnostic traversal and string limits. */
function compactResult(value: unknown): unknown {
    const source = object(value);
    if (!source || !Object.hasOwn(source, 'summary')) return diagnosticPreview(value);
    const { summary, ...metadata } = source;
    const result = diagnosticPreview(metadata) as Record<string, unknown>;
    if (typeof summary !== 'string') result.summary = diagnosticPreview(summary);
    else {
        const bytes = Buffer.byteLength(summary);
        result.summary = bytes <= COMPACT_SUMMARY_BYTES ? summary : {
            display_omitted: true, bytes, reason: 'compact summary exceeds 32768 byte limit',
        };
    }
    return result;
}

/** Every body gets a fresh traversal budget, independent of identity and its siblings. */
function boundedDiagnostic(value: unknown, maximum: number): unknown {
    const result = diagnosticPreview(value);
    return Buffer.byteLength(JSON.stringify(result)) <= maximum ? result : { display_omitted: true };
}

/** Encoded string allowance includes quotes and JSON escapes. */
function encodedPrefix(text: string, maximum: number): string {
    let low = 0;
    let high = Math.min(Buffer.byteLength(text), maximum - 2);
    const candidate = utf8Prefix(text, high);
    if (Buffer.byteLength(JSON.stringify(candidate)) <= maximum) return candidate;
    while (low < high) {
        const middle = Math.ceil((low + high) / 2);
        if (Buffer.byteLength(JSON.stringify(utf8Prefix(text, middle))) <= maximum) low = middle;
        else high = middle - 1;
    }
    return utf8Prefix(text, low);
}

/** Status and framework error fields survive unrelated metadata's traversal/byte limits. */
function annotations(value: unknown): unknown {
    const source = object(value);
    const result = boundedDiagnostic(value, 512);
    if (!source) return result;
    const projected = result as Record<string, unknown>;
    for (const key of ['status', 'loop_skipped']) {
        if (typeof source[key] === 'string' || typeof source[key] === 'boolean') {
            projected[key] = boundedDiagnostic(source[key], 256);
        }
    }
    const error = object(source.error);
    if (error) {
        const detail: Record<string, unknown> = {};
        for (const key of ['stage', 'message']) {
            const text = error[key];
            if (typeof text !== 'string') continue;
            detail[key] = encodedPrefix(text, key === 'stage' ? 128 : 1024);
            if (detail[key] !== text) detail.display_truncated = true;
        }
        projected.error = detail;
    }
    return projected;
}

/** Copy scalar correlation/classification directly, never by traversing argument trees. */
function toolCall(value: unknown, argumentBudget: number): Record<string, unknown> {
    const source = object(value) ?? {};
    const result: Record<string, unknown> = {};
    for (const key of ['id', 'name', 'type', 'security']) {
        if (typeof source[key] === 'string') result[key] = source[key];
    }
    if (source.arguments !== undefined) result.arguments = boundedDiagnostic(source.arguments, argumentBudget);
    if (source.extras !== undefined) result.extras = annotations(source.extras);
    return result;
}

/** Preserve output labels/text independently of native metadata. */
function toolContent(value: unknown, textBudget: number): unknown {
    const source = object(value);
    if (!source) return boundedDiagnostic(value, textBudget);
    const result: Record<string, unknown> = {};
    for (const key of ['type', 'modality']) {
        if (typeof source[key] === 'string') result[key] = source[key];
    }
    if (typeof source.raw === 'string') {
        result.raw = encodedPrefix(source.raw, textBudget);
        if (result.raw !== source.raw || source.truncated === true) {
            result.truncated = true;
            result.bytes = typeof source.bytes === 'number' ? source.bytes : Buffer.byteLength(source.raw);
        }
    }
    if (source.extras !== undefined) result.extras = boundedDiagnostic(source.extras, 512);
    return result;
}

/** Batch limits count represented calls; no early entry consumes a later entry's budget. */
function toolBatch(value: unknown, results: boolean): unknown {
    if (!Array.isArray(value)) return boundedDiagnostic(value, 1024);
    const entries: unknown[] = [];
    let omitted = 0;
    for (const item of value) {
        const previous = toolOmissionCount(item);
        if (previous > 0) omitted = Math.min(Number.MAX_SAFE_INTEGER, omitted + previous);
        else if (entries.length < 64) entries.push(item);
        else omitted = Math.min(Number.MAX_SAFE_INTEGER, omitted + 1);
    }
    const count = entries.length;
    const argumentBudget = Math.min(8192, Math.floor(64 * 1024 / Math.max(count, 1)));
    const outputBudget = Math.min(8192, Math.floor(128 * 1024 / Math.max(count, 1)));
    const projected = entries.map(item => {
        if (!results) return toolCall(item, argumentBudget);
        const source = object(item) ?? {};
        const result: Record<string, unknown> = {};
        for (const key of ['type', 'role']) {
            if (typeof source[key] === 'string') result[key] = source[key];
        }
        const record = object(source.invoke_return);
        function projectRecord(original: Record<string, unknown>, destination: Record<string, unknown>): void {
            if (original.query !== undefined) destination.query = toolCall(original.query, argumentBudget);
            if (original.output !== undefined) destination.output = toolContent(original.output, outputBudget);
            if (original.extras !== undefined) destination.extras = annotations(original.extras);
        }
        projectRecord(source, result);
        if (record) {
            const provenance: Record<string, unknown> = {};
            projectRecord(record, provenance);
            result.invoke_return = provenance;
        }
        if (Array.isArray(source.content)) {
            const parts = Math.min(source.content.length, 4);
            result.content = source.content.slice(0, parts).map(part => toolContent(part, Math.floor(outputBudget / parts)));
            const omitted = typeof source.omitted_parts === 'number' ? source.omitted_parts : 0;
            if (omitted || parts < source.content.length) result.omitted_parts = omitted + source.content.length - parts;
        }
        return result;
    });
    if (omitted > 0) projected.push({ display_omitted: true, omitted_items: omitted });
    return projected;
}

/** Preserve useful answers before allocating reasoning/metadata display space. */
function response(value: unknown, answerBudget: number): Record<string, unknown> {
    const source = object(value) ?? {};
    let remaining = answerBudget;
    const content: Record<string, unknown>[] = [];
    const original = Array.isArray(source.content) ? source.content : [];
    for (const item of original) {
        const part = object(item);
        if (!part || typeof part.raw !== 'string' || content.length >= 128 || remaining < 256) break;
        const project = (maximum: number): Record<string, unknown> => {
            const raw = utf8Prefix(part.raw as string, maximum);
            return { type: part.type, ...(part.modality === undefined ? {} : { modality: part.modality }), raw,
                ...(part.omitted === true ? { omitted: true } : {}),
                ...(raw !== part.raw || part.truncated === true
                    ? { truncated: true, bytes: typeof part.bytes === 'number' ? part.bytes : Buffer.byteLength(part.raw as string) } : {}) };
        };
        // Preserve exact answers that fit, including Unicode. Binary search a
        // bounded prefix only when actual JSON escaping exhausts the budget.
        let high = Math.min(Buffer.byteLength(part.raw), remaining - 256);
        let projected = project(high);
        if (Buffer.byteLength(JSON.stringify(projected)) > remaining) {
            let low = 0;
            while (low < high) {
                const middle = Math.ceil((low + high) / 2);
                if (Buffer.byteLength(JSON.stringify(project(middle))) <= remaining) low = middle;
                else high = middle - 1;
            }
            projected = project(low);
        }
        remaining -= Buffer.byteLength(JSON.stringify(projected));
        content.push(projected);
    }
    const result: Record<string, unknown> = { ...diagnosticPreview(source) as Record<string, unknown>, content };
    // Native provider extras often duplicate the complete response; never send them.
    delete result.extras;
    delete result.reasoning;
    if (source.invokes !== undefined) result.invokes = toolBatch(source.invokes, false);
    if (answerSource(source.answer_source)) result.answer_source = source.answer_source;
    else delete result.answer_source;
    const omitted = Number.isSafeInteger(source.omitted_parts) && Number(source.omitted_parts) > 0
        ? Number(source.omitted_parts) : 0;
    if (omitted || content.length < original.length) result.omitted_parts = omitted + original.length - content.length;
    const reasoning = object(source.reasoning);
    if (reasoning && typeof reasoning.raw === 'string') {
        const raw = utf8Prefix(reasoning.raw, REASONING_BYTES);
        result.reasoning = { type: reasoning.type, modality: reasoning.modality, raw,
            ...(reasoning.truncated === true || raw !== reasoning.raw
                ? { truncated: true, bytes: reasoning.bytes ?? Buffer.byteLength(reasoning.raw) } : {}) };
    }
    if (!answerSource(source.answer_source) && content.some(part => part.truncated)) result.answer_unavailable = 'older worker has no answer pagination source';
    return result;
}

/** Normalize before retaining or forwarding an accepted older-worker event. */
export function normalizeDisplay(event: string, value: unknown): unknown {
    if (event === 'history' && Buffer.byteLength(JSON.stringify(value)) <= 256 * 1024) return value;
    if (event === 'answer') return Buffer.byteLength(JSON.stringify(value)) <= 256 * 1024
        ? value : { request_id: object(value)?.request_id, display_omitted: true };
    let result: unknown;
    if (event === 'compact_finished') result = compactResult(value);
    else if (event === 'model_response') result = response(value, ANSWER_BYTES);
    else if (event === 'history') {
        const page = object(value) ?? {};
        // Built-in pages are already byte bounded; preserve answer sources/parts.
        result = { ...page, turns: Array.isArray(page.turns) ? page.turns.slice(0, 10).map(item => {
            const turn = object(item) ?? {};
            return { ...turn, user: response({ content: turn.user }, 24 * 1024).content, steps: Array.isArray(turn.steps)
                ? turn.steps.map(step => response(step, 96 * 1024)) : [] };
        }) : [] };
    } else if (event === 'tool_calls' || event === 'tool_results') {
        result = toolBatch(value, event === 'tool_results');
    } else result = diagnosticPreview(value);
    if (Buffer.byteLength(JSON.stringify(result)) <= MAX_ENCODED_DATA) return result;
    return { display_omitted: true, reason: 'display message exceeds encoded budget',
        ...(answerSource(object(value)?.answer_source) ? { answer_source: object(value)!.answer_source } : {}) };
}
