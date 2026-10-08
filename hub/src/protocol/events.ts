/**
 * @file worker event envelopes: validation and protocol vocabulary.
 *
 * The wire contract itself lives in docs/core/worker-protocol.md; this module
 * only encodes the parts the hub must act on — the envelope fields, the event
 * names core currently emits, and the handful of numbers that can exceed
 * JavaScript's safe integer range.
 *
 * Unknown event names and unknown extra fields are preserved rather than
 * rejected: a compatible hub must not disconnect merely because a future worker
 * emits an unfamiliar event (docs/core/worker-protocol.md, "Encoding and
 * message envelopes").
 */
import { normalizeDisplay, diagnosticPreview } from './display.ts';
import { validateSessionId } from '../state/session-id.ts';

/** Rendering hints for one event name. */
export interface EventMeta {
    tone: string;
    note: string;
    /** Delimits an invocation, which the panel groups into runs. */
    run?: boolean;
    /** Carries a state snapshot the panel caches. */
    snapshot?: boolean;
    /** `data` is an array rather than an object. */
    array?: boolean;
}

/**
 * Events emitted by core, with rendering hints for the panel.
 *
 * `data` is an object unless `array` says otherwise.
 */
export const EVENT_TABLE = {
    ready: { tone: 'info', note: 'worker startup finished' },
    status: { tone: 'info', note: 'state snapshot', snapshot: true },
    options: { tone: 'info', note: 'available choices and selections', snapshot: true },
    history: { tone: 'info', note: 'simplified conversation history page' },
    answer: { tone: 'info', note: 'read-only answer segment' },
    answer_error: { tone: 'warn', note: 'answer source unavailable' },
    history_error: { tone: 'warn', note: 'history query failed' },
    input_admitted: { tone: 'info', note: 'host admitted an input', run: true },
    input_rejected: { tone: 'warn', note: 'input failed validation or queue admission' },
    run_started: { tone: 'info', note: 'loop admitted the invocation', run: true },
    input_committed: { tone: 'info', note: 'user input integrated in memory' },
    model_response: { tone: 'assistant', note: 'committed model response display' },
    tool_calls: { tone: 'tool', note: 'calls proposed for a batch', array: true },
    tool_results: { tone: 'tool', note: 'returned batch display', array: true },
    persisted: { tone: 'muted', note: 'JSON snapshot written' },
    compact_finished: { tone: 'summary', note: 'context summary durably published' },
    export_error: { tone: 'warn', note: 'Markdown export failed' },
    error: { tone: 'error', note: 'control or storage diagnostic' },
    run_finished: { tone: 'summary', note: 'invocation settled', run: true },
} as const satisfies Record<string, EventMeta>;

/** An event name core emits today. */
export type KnownEvent = keyof typeof EVENT_TABLE;

/** Event names core emits today. */
export const KNOWN_EVENTS: string[] = Object.keys(EVENT_TABLE);

/** True when the event name is one the hub has rendering knowledge about. */
export function isKnownEvent(name: unknown): name is KnownEvent {
    return typeof name === 'string' && Object.hasOwn(EVENT_TABLE, name);
}

/** A 64-bit unsigned integer, kept alongside what it is safe to display as. */
export interface UnsignedInteger {
    /** The exact value, or null when the field was absent or unusable. */
    value: bigint | null;
    /** True when `value` round-trips through a JavaScript number. */
    safe: boolean;
    /** What to put on the wire: a number when it fits, else the raw text. */
    display: number | string | null;
}

/**
 * Recover a 64-bit unsigned integer without losing precision.
 *
 * `JSON.parse` rounds integers above 2^53, so when the parsed value is not a
 * safe integer the raw text is re-read with a targeted pattern. Sequence
 * numbers do not realistically reach that range today, but a hub that silently
 * mis-orders events is worse than a hub that costs one regex.
 */
export function readUnsignedInteger(
    parsed: unknown,
    rawText: string | undefined,
    field: string,
): UnsignedInteger {
    if (typeof parsed === 'number' && Number.isInteger(parsed) && parsed >= 0) {
        if (Number.isSafeInteger(parsed)) {
            return { value: BigInt(parsed), safe: true, display: parsed };
        }
    } else if (typeof parsed === 'string' && /^\d{1,20}$/.test(parsed)) {
        const value = BigInt(parsed);
        if (value > 18446744073709551615n) return { value: null, safe: false, display: null };
        const safe = value <= BigInt(Number.MAX_SAFE_INTEGER);
        return { value, safe, display: safe ? Number(value) : parsed };
    } else if (parsed !== undefined && parsed !== null) {
        return { value: null, safe: false, display: null };
    }
    if (typeof parsed !== 'number') return { value: null, safe: false, display: null };
    const match = new RegExp(`"${field}"\\s*:\\s*(\\d+)`).exec(rawText ?? '');
    if (!match) return { value: null, safe: false, display: null };
    if ((match[1] as string).length > 20) return { value: null, safe: false, display: null };
    const value = BigInt(match[1] as string);
    if (value > 18446744073709551615n) return { value: null, safe: false, display: null };
    const safe = value <= BigInt(Number.MAX_SAFE_INTEGER);
    return { value, safe, display: safe ? Number(value) : (match[1] as string) };
}

/** Envelope fields core always emits; a missing one is recorded, not fatal. */
const REQUIRED_ENVELOPE_FIELDS = [
    'event', 'session_id', 'worker_id', 'request_id', 'run_id', 'sequence',
] as const;

/**
 * A validated event envelope, normalised for the rest of the hub.
 *
 * The hub adds `hub_sequence`, `received_at`, `issues`, and `connection` before
 * forwarding it, so this is the shape as parsed rather than as sent.
 */
export interface ParsedEnvelope {
    type: 'event';
    event: string;
    session_id: string;
    worker_id: string;
    request_id: string;
    run_id: string;
    sequence: number | string | null;
    data: unknown;
    known: boolean;
    /** Wire size, measured once: a bounded transcript needs a cheap size. */
    bytes: number;
    /** Bounded envelope metadata, without a second copy of data. */
    raw: unknown;
}

/** A parsed envelope, or the reason it was refused. */
export type EnvelopeParseResult =
    | { ok: true; envelope: ParsedEnvelope; issues: string[]; sequence: UnsignedInteger }
    | { ok: false; error: string };

/** Parse one worker text message into a validated event envelope. */
export function parseEventEnvelope(text: string): EnvelopeParseResult {
    let parsed: unknown;
    try {
        parsed = JSON.parse(text);
    } catch (cause) {
        const message = cause instanceof Error ? cause.message : String(cause);
        return { ok: false, error: `invalid JSON: ${message}` };
    }
    if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
        return { ok: false, error: 'event message must be a JSON object' };
    }
    const document = parsed as Record<string, unknown>;
    if (document.type !== 'event') {
        return {
            ok: false,
            error: `expected envelope type "event", got ${JSON.stringify(document.type)}`,
        };
    }

    const issues: string[] = [];
    for (const field of REQUIRED_ENVELOPE_FIELDS) {
        if (!Object.hasOwn(document, field)) issues.push(`missing envelope field "${field}"`);
    }
    if (typeof document.event !== 'string' || document.event.length === 0) {
        return { ok: false, error: 'event name must be a nonempty string' };
    }
    for (const field of ['event', 'worker_id', 'request_id', 'run_id']) {
        if (typeof document[field] === 'string' && Buffer.byteLength(document[field] as string) > 128) {
            return { ok: false, error: `${field} exceeds the 128-byte identity budget` };
        }
    }
    if (typeof document.worker_id !== 'string' || document.worker_id.length === 0) {
        return { ok: false, error: 'worker_id must be a nonempty string' };
    }
    if (typeof document.session_id !== 'string') {
        return { ok: false, error: 'session_id must be a string' };
    }
    try {
        validateSessionId(document.session_id);
    } catch (cause) {
        const message = cause instanceof Error ? cause.message : String(cause);
        return { ok: false, error: `invalid session_id: ${message}` };
    }
    if (!Object.hasOwn(document, 'data')) issues.push('missing envelope field "data"');

    const sequence = readUnsignedInteger(document.sequence, text, 'sequence');
    const envelope: ParsedEnvelope = {
        type: 'event',
        event: document.event,
        session_id: document.session_id,
        worker_id: document.worker_id,
        request_id: typeof document.request_id === 'string' ? document.request_id : '',
        run_id: typeof document.run_id === 'string' ? document.run_id : '',
        sequence: sequence.display,
        data: normalizeDisplay(document.event, document.data ?? {}),
        known: isKnownEvent(document.event),
        // Wire size, measured once: a bounded transcript needs a cheap size and
        // re-serializing a large payload on every append is not cheap.
        bytes: 0,
        // Keep bounded diagnostic metadata without duplicating the event body.
        raw: diagnosticPreview(Object.fromEntries(Object.entries(document)
            .filter(([key]) => key !== 'data'))),
    };
    measureEnvelope(envelope);
    return { ok: true, envelope, issues, sequence };
}

/** Include the self-describing size field in the exact encoded byte count. */
export function measureEnvelope(envelope: { bytes?: number }): number {
    let size = Buffer.byteLength(JSON.stringify(envelope));
    while (envelope.bytes !== size) {
        envelope.bytes = size;
        size = Buffer.byteLength(JSON.stringify(envelope));
    }
    return size;
}
