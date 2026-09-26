/**
 * @file turning a worker envelope into something readable.
 *
 * The old panel gave every event a card of equal weight, so a conversation was
 * buried under `input_admitted`, `persisted` and `run_started` chips, and the
 * model's own words arrived as one `white-space: pre-wrap` block. This module
 * is the smallest honest separation of the two: an envelope is classified once,
 * into conversation or protocol, and the renderer draws what it is given.
 *
 * It is deliberately *not* the markdown renderer. That is the next stage, and
 * keeping the classification here means the markdown one will have a single
 * place to attach to rather than a switch statement inside a component.
 *
 * Untrusted content is never turned into markup: everything below returns
 * strings, and the components that draw them put them in text nodes.
 */
import type { WorkerEnvelope } from '../../../shared/protocol.ts';

/** One content part, as `Message.content` and `Result.output` carry it. */
interface ContentPart {
    type?: unknown;
    raw?: unknown;
    extras?: unknown;
}

/** One call the model proposed. */
export interface CallView {
    readonly id: string;
    readonly name: string;
    /** The arguments, as compact JSON. P5 renders `run_command` as a shell command. */
    readonly args: string;
    readonly security: string;
    readonly scheduling: string;
}

/** One returned result. */
export interface ResultView {
    readonly id: string;
    readonly name: string;
    readonly text: string;
    /** `extras.error`, when the tool framework annotated a failure. */
    readonly error: { readonly stage: string; readonly message: string } | null;
    /** `extras.loop_skipped`: not executed, which is not the same as failed. */
    readonly skipped: boolean;
}

/** What an envelope turned out to be. */
export type EnvelopeView =
    /** A complete model response: the conversation itself. */
    | {
        readonly kind: 'message';
        readonly text: string;
        readonly reasoning: string;
        readonly calls: readonly CallView[];
        readonly cost: string;
    }
    /** A batch of proposed calls. */
    | { readonly kind: 'calls'; readonly calls: readonly CallView[] }
    /** A batch of returned results. */
    | { readonly kind: 'results'; readonly results: readonly ResultView[] }
    /** An input was admitted. The text lives in the outbox, not on the wire. */
    | { readonly kind: 'admitted' }
    /** A run boundary or other bookkeeping. */
    | { readonly kind: 'protocol'; readonly label: string }
    /** Something went wrong, in the worker's own words. */
    | { readonly kind: 'problem'; readonly label: string; readonly text: string }
    /** An event name this panel has no renderer for. */
    | { readonly kind: 'unknown'; readonly label: string };

/** Read a field as a string, or `''`. */
function str(value: unknown): string {
    return typeof value === 'string' ? value : '';
}

/** Read a field as an object, or null. */
function obj(value: unknown): Record<string, unknown> | null {
    return typeof value === 'object' && value !== null && !Array.isArray(value)
        ? value as Record<string, unknown>
        : null;
}

/** Compact JSON for a value that may not be JSON at all. */
function compactJson(value: unknown): string {
    if (value === undefined) return '';
    try {
        return JSON.stringify(value) ?? String(value);
    } catch {
        return String(value);
    }
}

/**
 * The readable text of one content part.
 *
 * A reference is shown as its own URI and never fetched: the protocol says a
 * reference is data, and a panel that loaded it would turn a model's output
 * into a request the operator did not make. Binary is described rather than
 * decoded, which is what the old panel did and what the protocol asks for.
 */
export function contentText(part: unknown): string {
    if (typeof part === 'string') return part;
    const value = obj(part) as ContentPart | null;
    if (!value) return part === undefined || part === null ? '' : compactJson(part);
    const type = str(value.type) || 'unknown';
    const raw = typeof value.raw === 'string' ? value.raw : compactJson(value.raw);
    if (type === 'text') return raw;
    if (type === 'external_ref') return `external reference (not fetched): ${raw}`;
    if (type === 'binary') {
        const bytes = Math.floor((raw.length * 3) / 4);
        return `binary content: base64, ≈${bytes} bytes decoded`;
    }
    return `${type}: ${raw}`;
}

/** All content parts of a message or result, joined. */
function partsText(parts: unknown): string {
    if (!Array.isArray(parts)) return '';
    return parts.map(contentText).filter(Boolean).join('\n\n');
}

/** One call, normalised. */
function callView(value: unknown): CallView {
    const call = obj(value) ?? {};
    const args = call.arguments === undefined ? '' : compactJson(call.arguments);
    return {
        id: str(call.id),
        name: str(call.name) || '(unnamed call)',
        args,
        security: str(call.security),
        scheduling: str(call.type),
    };
}

/** One result, normalised. */
function resultView(value: unknown): ResultView {
    const result = obj(value) ?? {};
    const query = obj(result.query) ?? {};
    const extras = obj(result.extras) ?? {};
    const error = obj(extras.error);
    return {
        id: str(query.id),
        name: str(query.name) || '(unnamed call)',
        text: contentText(result.output),
        error: error
            ? { stage: str(error.stage) || 'unknown', message: str(error.message) }
            : null,
        skipped: extras.loop_skipped === true,
    };
}

/** The token cost line, keeping the protocol's own caveats about totals. */
function costLine(value: unknown): string {
    const cost = obj(value);
    if (!cost) return '';
    const bits: string[] = [];
    for (const key of ['prompt', 'generated', 'cache_hit']) {
        if (typeof cost[key] === 'number') bits.push(`${key} ${cost[key]}`);
    }
    if (bits.length === 0) return '';
    const prompt = typeof cost.prompt === 'number' ? cost.prompt : 0;
    const generated = typeof cost.generated === 'number' ? cost.generated : 0;
    // `cache_hit` counts tokens already inside `prompt`, so adding it again
    // would overstate the total — the protocol says so explicitly.
    bits.push(`total ${prompt + generated}`);
    return bits.join(' · ');
}

/** Classify one envelope. */
export function describeEnvelope(envelope: WorkerEnvelope): EnvelopeView {
    const name = typeof envelope.event === 'string' && envelope.event.length > 0
        ? envelope.event
        : '(unnamed event)';
    const data = envelope.data;

    switch (name) {
        case 'model_response': {
            const message = obj(data) ?? {};
            const invokes = Array.isArray(message.invokes)
                ? message.invokes.map(callView)
                : [];
            return {
                kind: 'message',
                text: partsText(message.content),
                reasoning: contentText(message.reasoning),
                calls: invokes,
                cost: costLine(message.cost),
            };
        }
        case 'tool_calls':
            return {
                kind: 'calls',
                calls: Array.isArray(data) ? data.map(callView) : [],
            };
        case 'tool_results':
            return {
                kind: 'results',
                results: Array.isArray(data) ? data.map(resultView) : [],
            };
        case 'input_admitted':
            return { kind: 'admitted' };
        case 'input_committed':
            return { kind: 'protocol', label: 'input integrated in memory' };
        case 'run_started':
            return { kind: 'protocol', label: 'run started' };
        case 'run_finished': {
            const summary = obj(data) ?? {};
            const status = str(summary.status) || 'settled';
            const exchanges = typeof summary.exchanges === 'number'
                ? ` · ${summary.exchanges} exchange(s)`
                : '';
            return { kind: 'protocol', label: `run finished: ${status}${exchanges}` };
        }
        case 'persisted': {
            const record = obj(data) ?? {};
            const boundary = str(record.boundary);
            return { kind: 'protocol', label: boundary ? `saved (${boundary})` : 'saved' };
        }
        case 'ready':
            return { kind: 'protocol', label: 'worker started' };
        case 'status':
            return { kind: 'protocol', label: 'status snapshot' };
        case 'options':
            return { kind: 'protocol', label: 'options snapshot' };
        case 'input_rejected': {
            const record = obj(data) ?? {};
            return {
                kind: 'problem',
                label: 'input rejected',
                text: str(record.message) || 'the worker refused this input',
            };
        }
        case 'error': {
            const record = obj(data) ?? {};
            return {
                kind: 'problem',
                label: 'worker diagnostic',
                text: str(record.message) || compactJson(data),
            };
        }
        case 'export_error': {
            const record = obj(data) ?? {};
            return {
                kind: 'problem',
                label: 'markdown export failed',
                text: str(record.message) || 'the optional export did not complete',
            };
        }
        default:
            return { kind: 'unknown', label: name };
    }
}

/** A short clock time for an envelope, or `''`. */
export function clockOf(envelope: WorkerEnvelope): string {
    const stamp = str(envelope.received_at);
    if (!stamp) return '';
    const date = new Date(stamp);
    if (Number.isNaN(date.getTime())) return '';
    return date.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
}
