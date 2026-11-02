/** Immutable, bounded Markdown snapshots; never cache AgentInputState or tool details. */
import { randomUUID } from 'node:crypto';
import { ToolFailure } from '../worker/tool-context.ts';

const MAX_BYTES = 64 * 1024 * 1024;
const PAGE_BYTES = 32 * 1024;
const TTL_MS = 120_000;
const mapping = (value: unknown): Record<string, unknown> | null =>
    value !== null && typeof value === 'object' && !Array.isArray(value)
        ? value as Record<string, unknown> : null;

/** Only visible text/reference parts enter the document. Reasoning, tools and extras do not. */
export function snapshotMarkdown(state: unknown): { text: string; blocks: number; omitted: boolean; latest: string | null } {
    const snapshot = mapping(state);
    if (!snapshot || !Array.isArray(snapshot.turns)) throw new ToolFailure('snapshot_unavailable', 'no saved conversation is available');
    const blocks: { id: string; title: string; text: string }[] = [];
    let bytes = 0;
    let omitted = false;
    let latest: string | null = null;
    const add = (message: unknown, id: string, title: string): void => {
        const item = mapping(message);
        if (!item || !Array.isArray(item.content)) throw new ToolFailure('snapshot_invalid', 'saved conversation has invalid message content');
        const parts: string[] = [];
        for (const value of item.content) {
            const part = mapping(value);
            if (!part || typeof part.raw !== 'string') throw new ToolFailure('snapshot_invalid', 'saved conversation has invalid content');
            if (!['text', 'external_ref'].includes(String(part.type))) { omitted = true; continue; }
            bytes += Buffer.byteLength(part.raw);
            if (bytes > MAX_BYTES) throw new ToolFailure('export_too_large', 'saved dialogue exceeds the 64 MiB export budget');
            parts.push(part.raw);
        }
        if (!parts.some(text => text.length)) return;
        if (blocks.length >= 100_000) throw new ToolFailure('export_too_large', 'too many dialogue blocks');
        blocks.push({ id, title, text: parts.join('\n') });
        if (id.includes('-response-')) latest = id;
    };
    snapshot.turns.forEach((value, index) => {
        const turn = mapping(value);
        if (!turn || !Array.isArray(turn.agent_loop_step)) throw new ToolFailure('snapshot_invalid', 'invalid saved turn');
        const name = `turn-${String(index + 1).padStart(6, '0')}`;
        const user = mapping(turn.user_input);
        if (mapping(user?.extras)?.['simplex.internal_input'] !== 'auto_compact_continue')
            add(turn.user_input, `${name}-request`, `Turn ${index + 1} — Request`);
        turn.agent_loop_step.forEach((value, step) => {
            const entry = mapping(value);
            add(entry?.model_response, `${name}-response-${step + 1}`, `Turn ${index + 1} — Response ${step + 1}`);
        });
    });
    let document = randomUUID();
    while (blocks.some(block => block.text.includes(`simplex:block:${document}:`))) document = randomUUID();
    const marker = (id: string): string => `simplex:block:${document}:${id}`;
    const text = ['# Subagent conversation', '',
        'Source: latest persisted snapshot. Running work may not yet be saved.',
        'Reasoning and tool calls are excluded; compacted-away history is not included.',
        ...(omitted ? ['Some non-text content is omitted.'] : []), '', '## Contents', '',
        ...blocks.map(block => `- ${marker(block.id)}`), '',
        ...blocks.flatMap(block => [`<!-- ${marker(block.id)}:begin -->`, `## ${block.title}`, '',
            block.text, '', `<!-- ${marker(block.id)}:end -->`, ''])].join('\n');
    if (Buffer.byteLength(text) > MAX_BYTES) throw new ToolFailure('export_too_large', 'formatted dialogue exceeds the 64 MiB export budget');
    return { text, blocks: blocks.length, omitted, latest: latest ? marker(latest) : null };
}

interface Snapshot {
    owner: string; bytes: Buffer; blocks: number; omitted: boolean; latest: string | null;
    expires: number;
}

/** Two concurrent exports per Hub; reservations include asynchronous manager reads. */
export class ReceiveExports {
    private snapshots = new Map<string, Snapshot>();
    private pending = 0;
    private closed = false;
    private timer = setInterval(() => this.prune(), 10_000).unref();
    private prune(): void {
        for (const [key, value] of this.snapshots) if (value.expires <= Date.now()) this.snapshots.delete(key);
    }
    async start(owner: string, load: () => Promise<unknown>, validate: () => void): Promise<Record<string, unknown>> {
        this.prune();
        if (this.closed || this.snapshots.size + this.pending >= 2) throw new ToolFailure('export_busy', 'export capacity exhausted; retry later');
        this.pending++;
        try {
            const state = await load();
            validate();
            if (this.closed) throw new ToolFailure('export_unavailable', 'Hub is stopping');
            const rendered = snapshotMarkdown(state);
            const token = randomUUID();
            this.snapshots.set(token, { owner, bytes: Buffer.from(rendered.text), blocks: rendered.blocks,
                omitted: rendered.omitted, latest: rendered.latest, expires: Date.now() + TTL_MS });
            return this.page(owner, { token, offset: 0 });
        } finally { this.pending--; }
    }
    page(owner: string, args: Record<string, unknown>): Record<string, unknown> {
        this.prune();
        const value = typeof args.token === 'string' ? this.snapshots.get(args.token) : undefined;
        if (!value || value.owner !== owner) throw new ToolFailure('export_expired', 'export snapshot expired or belongs to another run');
        if (!Number.isSafeInteger(args.offset) || (args.offset as number) < 0 || (args.offset as number) >= value.bytes.length)
            throw new ToolFailure('invalid_arguments', 'invalid export offset');
        const offset = args.offset as number;
        if ((value.bytes[offset]! & 0xc0) === 0x80) throw new ToolFailure('invalid_arguments', 'offset splits UTF-8');
        let next = Math.min(offset + PAGE_BYTES, value.bytes.length);
        while (next < value.bytes.length && (value.bytes[next]! & 0xc0) === 0x80) next--;
        const result = { token: args.token, offset, next, total: value.bytes.length,
            text: value.bytes.subarray(offset, next).toString('utf8'), done: next === value.bytes.length,
            blocks: value.blocks, omitted: value.omitted, latest_block: value.latest, source: 'persisted_snapshot' };
        if (result.done) this.snapshots.delete(args.token as string);
        return result;
    }
    close(): void { this.closed = true; clearInterval(this.timer); this.snapshots.clear(); }
}
