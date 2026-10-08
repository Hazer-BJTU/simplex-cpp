import { answerSource } from '../../shared/answers.ts';
/** Display copies only: executable inputs and approval authority never use this module. */
const MAX_ENCODED_DATA = 768 * 1024;
const ANSWER_BYTES = 512 * 1024;
const REASONING_BYTES = 4096;
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
    if (event === 'model_response') result = response(value, ANSWER_BYTES);
    else if (event === 'history') {
        const page = object(value) ?? {};
        // Built-in pages are already byte bounded; preserve answer sources/parts.
        result = { ...page, turns: Array.isArray(page.turns) ? page.turns.slice(0, 10).map(item => {
            const turn = object(item) ?? {};
            return { ...turn, user: response({ content: turn.user }, 24 * 1024).content, steps: Array.isArray(turn.steps)
                ? turn.steps.map(step => response(step, 96 * 1024)) : [] };
        }) : [] };
    } else result = diagnosticPreview(value);
    if (Buffer.byteLength(JSON.stringify(result)) <= MAX_ENCODED_DATA) return result;
    return { display_omitted: true, reason: 'display message exceeds encoded budget',
        ...(answerSource(object(value)?.answer_source) ? { answer_source: object(value)!.answer_source } : {}) };
}
