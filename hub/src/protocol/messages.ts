/**
 * @file outbound worker messages: payload and signal builders.
 *
 * Every rule enforced here also exists on the worker side; the hub duplicates
 * the cheap ones so the panel reports a mistake immediately instead of after a
 * round trip. The worker remains authoritative — an `input_rejected` event is
 * still surfaced as-is, and this module never rewrites a settled request.
 */
import { randomUUID } from 'node:crypto';

/** Input content encodings accepted by the worker. */
export const CONTENT_TYPES = ['text', 'binary', 'external_ref'] as const;

/** Confirmation policies selectable per payload. */
export const CONFIRMATION_MODES = ['ask', 'approve', 'deny'] as const;

/** Signal operations accepted by the worker. */
export const SIGNAL_OPERATIONS = ['status', 'options', 'cancel', 'shutdown'] as const;

/** Input operations accepted by the worker. */
export const INPUT_OPERATIONS = ['message', 'continue', 'history'] as const;

/** One accepted content encoding. */
export type ContentType = (typeof CONTENT_TYPES)[number];
/** One confirmation policy. */
export type ConfirmationMode = (typeof CONFIRMATION_MODES)[number];
/** One signal operation. */
export type SignalOperation = (typeof SIGNAL_OPERATIONS)[number];
/** One input operation. */
export type InputOperation = (typeof INPUT_OPERATIONS)[number];

/** Fields the worker rejects inside a payload's `data`. */
const FORBIDDEN_DATA_FIELDS = ['role', 'invokes', 'invoke_return', 'type'] as const;

/** A content part after validation. */
export interface NormalizedContentPart {
    type: ContentType;
    raw: string;
    extras?: Record<string, unknown>;
}

/** Payload options after validation. */
export interface NormalizedOptions {
    model?: Record<string, unknown>;
    /** Reserved by the worker protocol, and required to be empty. */
    tools?: Record<string, unknown>;
    confirmation?: { mode?: ConfirmationMode };
}

/** The `data` of a payload envelope. */
export interface PayloadData {
    operation: InputOperation;
    request_id: string;
    content?: NormalizedContentPart[];
    options?: NormalizedOptions;
    start?: number;
    step?: number;
    limit?: number;
}

/** A payload envelope, ready to write to a worker socket. */
export interface PayloadEnvelope {
    type: 'payload';
    data: PayloadData;
}

/** A signal envelope, ready to write to a worker socket. */
export interface SignalEnvelope {
    type: 'signal';
    data: { operation: SignalOperation; run_id?: string };
}

/** The identifiers a confirmation response has to echo back. */
export interface ConfirmationCorrelation {
    worker_id: string;
    session_id: string;
    run_id: string;
    confirmation_id: string;
}

/** A confirmation response, ready to write to a confirmation socket. */
export interface ConfirmationResponseEnvelope {
    type: 'confirmation_response';
    data: ConfirmationCorrelation & { decision: 'approved' | 'denied'; reason: string };
}

/** Error raised for a message the worker would reject. */
export class ProtocolError extends Error {
    constructor(message: string) {
        super(message);
        this.name = 'ProtocolError';
    }
}

/** A fresh request identifier: nonempty, at most 128 UTF-8 bytes. */
export function newRequestId(): string {
    return randomUUID();
}

/** UTF-8 length in bytes, which is what the 128-byte request ID limit counts. */
export function utf8Length(text: string): number {
    return Buffer.byteLength(text, 'utf8');
}

/** Validate one content part, returning a normalized copy. */
export function normalizeContentPart(part: unknown, index: number): NormalizedContentPart {
    if (typeof part !== 'object' || part === null || Array.isArray(part)) {
        throw new ProtocolError(`content[${index}] must be an object`);
    }
    const { type, raw, extras } = part as Record<string, unknown>;
    if (!(CONTENT_TYPES as readonly unknown[]).includes(type)) {
        throw new ProtocolError(
            `content[${index}].type must be one of ${CONTENT_TYPES.join(', ')}`
            + (type === 'image' ? ' (use external_ref with the image URL in raw)' : ''));
    }
    if (typeof raw !== 'string' || raw.length === 0) {
        throw new ProtocolError(`content[${index}].raw must be a nonempty string`);
    }
    const normalized: NormalizedContentPart = { type: type as ContentType, raw };
    if (extras !== undefined) {
        if (typeof extras !== 'object' || extras === null || Array.isArray(extras)) {
            throw new ProtocolError(`content[${index}].extras must be an object`);
        }
        normalized.extras = extras as Record<string, unknown>;
    }
    return normalized;
}

/**
 * Validate optional payload `options`: `model` (provider keys), `tools`
 * (reserved, must be empty), `confirmation.mode`.
 */
export function normalizeOptions(options: unknown): NormalizedOptions | undefined {
    if (options === undefined || options === null) return undefined;
    if (typeof options !== 'object' || Array.isArray(options)) {
        throw new ProtocolError('options must be an object');
    }
    const normalized: NormalizedOptions = {};
    for (const [category, value] of Object.entries(options as Record<string, unknown>)) {
        if (category === 'model' || category === 'confirmation') {
            if (typeof value !== 'object' || value === null || Array.isArray(value)) {
                throw new ProtocolError(`options.${category} must be an object`);
            }
            if (category === 'confirmation') {
                const mode = (value as Record<string, unknown>).mode;
                if (mode !== undefined && !(CONFIRMATION_MODES as readonly unknown[]).includes(mode)) {
                    throw new ProtocolError(
                        `options.confirmation.mode must be one of ${CONFIRMATION_MODES.join(', ')}`);
                }
                normalized.confirmation = mode === undefined
                    ? {}
                    : { mode: mode as ConfirmationMode };
            } else {
                normalized.model = value as Record<string, unknown>;
            }
            continue;
        }
        if (category === 'tools') {
            if (typeof value !== 'object' || value === null || Array.isArray(value)) {
                throw new ProtocolError('options.tools must be an object');
            }
            if (Object.keys(value).length > 0) {
                throw new ProtocolError('options.tools is reserved and must be empty');
            }
            normalized.tools = {};
            continue;
        }
        throw new ProtocolError(`unknown options category "${category}"`);
    }
    return normalized;
}

/** What `buildPayload` accepts. */
export interface PayloadInput {
    operation?: InputOperation;
    requestId: string;
    /** Required for `message`, refused for `continue`. */
    content?: unknown;
    options?: unknown;
    start?: number;
    step?: number;
    limit?: number;
}

/** Build a `payload` envelope for a user message or a continuation. */
export function buildPayload({
    operation = 'message', requestId, content, options, start, step, limit,
}: PayloadInput): PayloadEnvelope {
    if (!(INPUT_OPERATIONS as readonly unknown[]).includes(operation)) {
        throw new ProtocolError(`operation must be one of ${INPUT_OPERATIONS.join(', ')}`);
    }
    if (typeof requestId !== 'string' || requestId.length === 0) {
        throw new ProtocolError('request_id must be a nonempty string');
    }
    if (utf8Length(requestId) > 128) {
        throw new ProtocolError('request_id must be at most 128 UTF-8 bytes');
    }
    const data: PayloadData = { operation, request_id: requestId };

    if (operation === 'history') {
        if (content !== undefined || options !== undefined) {
            throw new ProtocolError('history must not carry content or options');
        }
        if (start !== undefined) {
            if (!Number.isSafeInteger(start) || start < 0) {
                throw new ProtocolError('history start must be a nonnegative safe integer');
            }
            data.start = start;
        }
        if (step !== undefined) {
            if (!Number.isSafeInteger(step) || step < 0) {
                throw new ProtocolError('history step must be a nonnegative safe integer');
            }
            data.step = step;
        }
        if (limit !== undefined) {
            if (!Number.isSafeInteger(limit) || limit < 1 || limit > 10) {
                throw new ProtocolError('history limit must be between 1 and 10');
            }
            data.limit = limit;
        }
    } else if (operation === 'continue') {
        if (content !== undefined && content !== null) {
            throw new ProtocolError('continue must not carry content');
        }
    } else {
        if (!Array.isArray(content) || content.length === 0) {
            throw new ProtocolError('content must be a nonempty array of parts');
        }
        data.content = content.map((part, index) => normalizeContentPart(part, index));
    }

    if (operation !== 'history') {
        const normalized = normalizeOptions(options);
        if (normalized !== undefined) data.options = normalized;
    }
    for (const field of FORBIDDEN_DATA_FIELDS) {
        if (Object.hasOwn(data, field)) {
            throw new ProtocolError(`"${field}" is not accepted in payload data`);
        }
    }
    return { type: 'payload', data };
}

/** What `buildSignal` accepts. */
export interface SignalInput {
    operation: SignalOperation;
    /** Required for `cancel`; absent for every other operation. */
    runId?: string | undefined;
}

/** Build a `signal` envelope. */
export function buildSignal({ operation, runId }: SignalInput): SignalEnvelope {
    if (!(SIGNAL_OPERATIONS as readonly unknown[]).includes(operation)) {
        throw new ProtocolError(`signal operation must be one of ${SIGNAL_OPERATIONS.join(', ')}`);
    }
    if (operation === 'cancel') {
        if (typeof runId !== 'string' || runId.length === 0) {
            throw new ProtocolError('cancel requires a nonempty run_id');
        }
        return { type: 'signal', data: { operation, run_id: runId } };
    }
    return { type: 'signal', data: { operation } };
}

/**
 * Build a one-shot `confirmation_response` for a received request.
 *
 * All four identifiers are echoed exactly as received: the worker rejects a
 * mismatch, and a decision is only valid for the request it answers.
 */
export function buildConfirmationResponse(
    request: ConfirmationCorrelation,
    decision: 'approved' | 'denied',
    reason = 'operator decision',
): ConfirmationResponseEnvelope {
    if (decision !== 'approved' && decision !== 'denied') {
        throw new ProtocolError('decision must be approved or denied');
    }
    if (typeof reason !== 'string') {
        throw new ProtocolError('reason must be a string');
    }
    return {
        type: 'confirmation_response',
        data: {
            worker_id: request.worker_id,
            session_id: request.session_id,
            run_id: request.run_id,
            confirmation_id: request.confirmation_id,
            decision,
            reason,
        },
    };
}
