/** Read-only answer pagination shared by Hub and panel; offsets count UTF-8 bytes. */
export interface AnswerSource {
    worker_id: string;
    turn: number;
    step: number;
    commit_sequence: string;
    /** New workers require this; older compatible sources can omit it. */
    fingerprint?: string;
}
export interface AnswerQuery { source: AnswerSource; part: number; offset: number }
export interface AnswerPage extends AnswerQuery {
    request_id: string;
    raw: string;
    type: string;
    modality: string;
    bytes: number;
    next_offset: number;
    next_part: number;
    total_parts: number;
    done: boolean;
}
const record = (value: unknown): value is Record<string, unknown> =>
    value !== null && typeof value === 'object' && !Array.isArray(value);
const index = (value: unknown): value is number => Number.isSafeInteger(value) && (value as number) >= 0;
export function answerSource(value: unknown): value is AnswerSource {
    return record(value) && Object.keys(value).every(key => ['worker_id', 'turn', 'step', 'commit_sequence', 'fingerprint'].includes(key))
        && (Object.keys(value).length === 4 || Object.keys(value).length === 5)
        && typeof value.worker_id === 'string' && value.worker_id.length > 0 && value.worker_id.length <= 128
        && index(value.turn) && index(value.step) && typeof value.commit_sequence === 'string'
        && /^[1-9][0-9]{0,19}$/.test(value.commit_sequence)
        && (value.fingerprint === undefined || typeof value.fingerprint === 'string' && /^[a-f0-9]{64}$/.test(value.fingerprint));
}
export function sameSource(a: AnswerSource, b: AnswerSource): boolean {
    return a.worker_id === b.worker_id && a.turn === b.turn && a.step === b.step && a.commit_sequence === b.commit_sequence && a.fingerprint === b.fingerprint;
}
export function answerQuery(value: unknown): value is AnswerQuery {
    return record(value) && Object.keys(value).length === 3 && answerSource(value.source)
        && index(value.part) && index(value.offset);
}
/** Verify correlation and forward progress before accepting any page bytes. */
export function answerPage(value: unknown, query: AnswerQuery): value is AnswerPage {
    if (!record(value) || !answerSource(value.source) || !sameSource(value.source, query.source)
        || value.part !== query.part || value.offset !== query.offset || typeof value.request_id !== 'string'
        || typeof value.raw !== 'string' || typeof value.type !== 'string' || typeof value.modality !== 'string'
        || !index(value.bytes) || !index(value.total_parts) || query.part >= value.total_parts
        || !index(value.next_offset) || !index(value.next_part) || typeof value.done !== 'boolean') return false;
    const length = new TextEncoder().encode(value.raw).length;
    if (length > 32768 || value.next_offset !== query.offset + length || value.next_offset > value.bytes) return false;
    const finished = value.next_offset === value.bytes;
    return value.next_part === query.part + (finished ? 1 : 0)
        && value.done === (finished && value.next_part === value.total_parts)
        && (finished || length > 0);
}
