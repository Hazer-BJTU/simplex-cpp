/**
 * @file reading values out of worker payloads, and formatting them for people.
 *
 * Everything here returns strings or plain objects. Nothing builds markup, and
 * nothing decides what a payload *means* — that is `rounds.ts`'s job. This
 * module only answers "what did the worker actually send", which is the part
 * that is easy to get subtly wrong: the worker protocol documents one shape for
 * a tool result and core currently projects another, and a reader that knows
 * only the documented one shows `(unnamed call)` and no output at all.
 */
import type { WorkerEnvelope } from '../../../shared/protocol.ts';

/** Read a field as a string, or `''`. */
export function str(value: unknown): string {
    return typeof value === 'string' ? value : '';
}

/** Read a field as an object, or null. Arrays count as not-an-object here. */
export function obj(value: unknown): Record<string, unknown> | null {
    return typeof value === 'object' && value !== null && !Array.isArray(value)
        ? value as Record<string, unknown>
        : null;
}

/** Compact JSON for a value that may not be JSON at all. */
export function compactJson(value: unknown): string {
    if (value === undefined) return '';
    try {
        return JSON.stringify(value) ?? String(value);
    } catch {
        return String(value);
    }
}

/** Pretty JSON, for the raw escape hatches. */
export function prettyJson(value: unknown): string {
    if (value === undefined) return '';
    try {
        return JSON.stringify(value, null, 2) ?? String(value);
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
 * decoded, which is what the protocol asks for.
 */
export function contentText(part: unknown): string {
    if (typeof part === 'string') return part;
    const value = obj(part);
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

/** All content parts of a message, joined as separate paragraphs. */
export function partsText(parts: unknown): string {
    if (!Array.isArray(parts)) return '';
    return parts.map(contentText).filter(Boolean).join('\n\n');
}

/** A short clock time for an envelope, or `''`. */
export function clockOf(envelope: WorkerEnvelope): string {
    const stamp = str(envelope.received_at);
    if (!stamp) return '';
    const date = new Date(stamp);
    if (Number.isNaN(date.getTime())) return '';
    return date.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
}

/** Milliseconds since an ISO timestamp, or null when it cannot be read. */
export function sinceMs(envelope: WorkerEnvelope, from: number): number | null {
    const stamp = str(envelope.received_at);
    if (!stamp) return null;
    const at = Date.parse(stamp);
    if (Number.isNaN(at)) return null;
    return Math.max(0, at - from);
}

/** A duration a person can read at a glance. */
export function formatDuration(ms: number | null): string {
    if (ms === null) return '';
    if (ms < 1000) return `${Math.round(ms)}ms`;
    if (ms < 60_000) return `${(ms / 1000).toFixed(ms < 10_000 ? 1 : 0)}s`;
    const minutes = Math.floor(ms / 60_000);
    const seconds = Math.round((ms % 60_000) / 1000);
    return `${minutes}m ${seconds}s`;
}

/**
 * A code fence long enough to hold `code` without being closed early.
 *
 * CommonMark closes a fence only on a run of at least as many backticks as
 * opened it, so growing past the longest run inside the content makes the fence
 * unbreakable. A command containing ``` would otherwise end the block and the
 * rest of it would be parsed as markdown — which is how untrusted text becomes
 * structure.
 */
export function fenceFor(code: string): string {
    const longest = [...code.matchAll(/`+/g)].reduce(
        (longestRun, match) => Math.max(longestRun, match[0].length), 0,
    );
    return '`'.repeat(Math.max(3, longest + 1));
}

/** One call the model proposed, as the panel shows it. */
export interface CallView {
    readonly id: string;
    readonly name: string;
    readonly args: unknown;
    readonly security: string;
    readonly scheduling: string;
}

/** One returned result, as the panel shows it. */
export interface ResultView {
    readonly id: string;
    readonly name: string;
    /** The result's readable text, exactly as the worker rendered it. */
    readonly text: string;
    readonly security: string;
    readonly error: { readonly stage: string; readonly message: string } | null;
    /** `extras.loop_skipped`: not executed, which is not the same as failed. */
    readonly skipped: boolean;
}

/** Read one call object. */
export function callView(value: unknown): CallView {
    const call = obj(value) ?? {};
    return {
        id: str(call.id),
        name: str(call.name) || '(unnamed call)',
        args: call.arguments,
        security: str(call.security),
        scheduling: str(call.type),
    };
}

/**
 * Read one entry of a `tool_results` batch.
 *
 * The worker protocol documents a Result object (`{query, output, extras}`).
 * Core currently projects results as tool *messages* instead — `{content,
 * invoke_return, role, type}`, with the provenance nested under
 * `invoke_return`. Both are accepted, because a reader that knows only the
 * documented one shows no call name, no arguments and no output, which is
 * exactly what this panel did before this stage.
 *
 * The settled call is preferred over the proposed one: `tool_results.query` has
 * been through security evaluation, while the same call in `tool_calls` had not.
 */
export function resultView(value: unknown): ResultView {
    const entry = obj(value) ?? {};
    const provenance = obj(entry.invoke_return) ?? {};
    const query = obj(entry.query) ?? obj(provenance.query) ?? {};
    const output = entry.output ?? provenance.output ?? null;
    const extras = obj(entry.extras) ?? obj(provenance.extras) ?? {};
    const error = obj(extras.error);
    const text = output !== null ? contentText(output) : partsText(entry.content);
    return {
        id: str(query.id),
        name: str(query.name) || str(entry.type) || '(unnamed result)',
        text,
        security: str(query.security),
        error: error
            ? { stage: str(error.stage) || 'unknown', message: str(error.message) }
            : null,
        skipped: extras.loop_skipped === true,
    };
}

/**
 * The token cost line.
 *
 * `cache_hit` counts tokens already inside `prompt`, so adding it again would
 * overstate the total — the protocol says so explicitly.
 */
export function costLine(value: unknown): { text: string; total: number | null } {
    const cost = obj(value);
    if (!cost) return { text: '', total: null };
    const bits: string[] = [];
    for (const key of ['prompt', 'generated', 'cache_hit']) {
        if (typeof cost[key] === 'number') bits.push(`${key} ${cost[key]}`);
    }
    if (bits.length === 0) return { text: '', total: null };
    const prompt = typeof cost.prompt === 'number' ? cost.prompt : 0;
    const generated = typeof cost.generated === 'number' ? cost.generated : 0;
    const total = prompt + generated;
    bits.push(`total ${total}`);
    return { text: bits.join(' · '), total };
}
