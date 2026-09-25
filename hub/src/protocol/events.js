/**
 * @file worker event envelopes: validation and protocol vocabulary.
 *
 * The wire contract itself lives in core/docs/worker-protocol.md; this module
 * only encodes the parts the hub must act on — the envelope fields, the event
 * names core currently emits, and the handful of numbers that can exceed
 * JavaScript's safe integer range.
 *
 * Unknown event names and unknown extra fields are preserved rather than
 * rejected: a compatible hub must not disconnect merely because a future worker
 * emits an unfamiliar event (core/docs/worker-protocol.md, "Encoding and
 * message envelopes").
 */
import { validateSessionId } from '../state/session-id.js';

/**
 * Events emitted by core, with rendering hints for the panel.
 *
 * `data` is `object` unless noted. `run` marks events that delimit an
 * invocation, which the panel uses to group a transcript into runs.
 */
export const EVENT_TABLE = {
    ready: { tone: 'info', note: 'worker startup finished' },
    status: { tone: 'info', note: 'state snapshot', snapshot: true },
    options: { tone: 'info', note: 'available choices and selections', snapshot: true },
    input_admitted: { tone: 'info', note: 'host admitted an input', run: true },
    input_rejected: { tone: 'warn', note: 'dequeued input failed validation' },
    run_started: { tone: 'info', note: 'loop admitted the invocation', run: true },
    input_committed: { tone: 'info', note: 'user input integrated in memory' },
    model_response: { tone: 'assistant', note: 'complete model response' },
    tool_calls: { tone: 'tool', note: 'calls proposed for a batch', array: true },
    tool_results: { tone: 'tool', note: 'complete returned batch', array: true },
    persisted: { tone: 'muted', note: 'JSON snapshot written' },
    export_error: { tone: 'warn', note: 'Markdown export failed' },
    error: { tone: 'error', note: 'control or storage diagnostic' },
    run_finished: { tone: 'summary', note: 'invocation settled', run: true },
};

/** Event names core emits today. */
export const KNOWN_EVENTS = Object.keys(EVENT_TABLE);

/** True when the event name is one the hub has rendering knowledge about. */
export function isKnownEvent(name) {
    return Object.hasOwn(EVENT_TABLE, name);
}

/**
 * Recover a 64-bit unsigned integer without losing precision.
 *
 * `JSON.parse` rounds integers above 2^53, so when the parsed value is not a
 * safe integer the raw text is re-read with a targeted pattern. Sequence
 * numbers do not realistically reach that range today, but a hub that silently
 * mis-orders events is worse than a hub that costs one regex.
 *
 * @returns {{value: bigint|null, safe: boolean, display: number|string|null}}
 */
export function readUnsignedInteger(parsed, rawText, field) {
    if (typeof parsed === 'number' && Number.isInteger(parsed) && parsed >= 0) {
        if (Number.isSafeInteger(parsed)) {
            return { value: BigInt(parsed), safe: true, display: parsed };
        }
    } else if (typeof parsed === 'string' && /^\d+$/.test(parsed)) {
        const value = BigInt(parsed);
        return {
            value,
            safe: value <= BigInt(Number.MAX_SAFE_INTEGER),
            display: value <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(value) : parsed,
        };
    } else if (parsed !== undefined && parsed !== null) {
        return { value: null, safe: false, display: null };
    }
    if (typeof parsed !== 'number') return { value: null, safe: false, display: null };
    const match = new RegExp(`"${field}"\\s*:\\s*(\\d+)`).exec(rawText ?? '');
    if (!match) return { value: null, safe: false, display: null };
    const value = BigInt(match[1]);
    return {
        value,
        safe: value <= BigInt(Number.MAX_SAFE_INTEGER),
        display: value <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(value) : match[1],
    };
}

/** Envelope fields core always emits; a missing one is recorded, not fatal. */
const REQUIRED_ENVELOPE_FIELDS = [
    'event', 'session_id', 'worker_id', 'request_id', 'run_id', 'sequence',
];

/**
 * Parse one worker text message into a validated event envelope.
 *
 * @param {string} text complete WebSocket text message.
 * @returns {{ok: true, envelope: object, issues: string[]}
 *          |{ok: false, error: string}}
 */
export function parseEventEnvelope(text) {
    let parsed;
    try {
        parsed = JSON.parse(text);
    } catch (cause) {
        return { ok: false, error: `invalid JSON: ${cause.message}` };
    }
    if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
        return { ok: false, error: 'event message must be a JSON object' };
    }
    if (parsed.type !== 'event') {
        return { ok: false, error: `expected envelope type "event", got ${JSON.stringify(parsed.type)}` };
    }

    const issues = [];
    for (const field of REQUIRED_ENVELOPE_FIELDS) {
        if (!Object.hasOwn(parsed, field)) issues.push(`missing envelope field "${field}"`);
    }
    if (typeof parsed.event !== 'string' || parsed.event.length === 0) {
        return { ok: false, error: 'event name must be a nonempty string' };
    }
    if (typeof parsed.worker_id !== 'string' || parsed.worker_id.length === 0) {
        return { ok: false, error: 'worker_id must be a nonempty string' };
    }
    if (typeof parsed.session_id !== 'string') {
        return { ok: false, error: 'session_id must be a string' };
    }
    try {
        validateSessionId(parsed.session_id);
    } catch (cause) {
        return { ok: false, error: `invalid session_id: ${cause.message}` };
    }
    if (!Object.hasOwn(parsed, 'data')) issues.push('missing envelope field "data"');

    const sequence = readUnsignedInteger(parsed.sequence, text, 'sequence');
    const envelope = {
        type: 'event',
        event: parsed.event,
        session_id: parsed.session_id,
        worker_id: parsed.worker_id,
        request_id: typeof parsed.request_id === 'string' ? parsed.request_id : '',
        run_id: typeof parsed.run_id === 'string' ? parsed.run_id : '',
        sequence: sequence.display,
        data: parsed.data ?? {},
        known: isKnownEvent(parsed.event),
        // Wire size, measured once: a bounded transcript needs a cheap size and
        // re-serializing a large payload on every append is not cheap.
        bytes: text.length,
        // Everything the worker sent, so the panel's raw view and any future
        // extension field survive untouched.
        raw: parsed,
    };
    return { ok: true, envelope, issues, sequence };
}
