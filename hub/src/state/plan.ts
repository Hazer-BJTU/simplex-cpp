/** Session plans are hub-owned documents, independent of worker conversation history. */
import { randomUUID } from 'node:crypto';
import { closeSync, constants, existsSync, fstatSync, fsyncSync, mkdirSync, openSync, readSync, renameSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import type { SessionPlan } from '../../shared/protocol.ts';
import { validateSessionId } from './session-id.ts';

export const MAX_PLAN_BYTES = 64 * 1024;
export const emptyPlan = (): SessionPlan => ({ markdown: '', revision: 0, updated_at: null });

/** Reject lone UTF-16 surrogates as well as oversized UTF-8 text. */
export function validMarkdown(value: unknown): value is string {
    return typeof value === 'string' && Buffer.byteLength(value) <= MAX_PLAN_BYTES
        && Buffer.from(value, 'utf8').toString('utf8') === value;
}

/** Validate persisted and wire snapshots before using them. */
export function validPlan(value: unknown): value is SessionPlan {
    if (!value || typeof value !== 'object') return false;
    const plan = value as SessionPlan;
    return validMarkdown(plan.markdown)
        && Number.isSafeInteger(plan.revision) && plan.revision >= 0
        && (plan.updated_at === null || typeof plan.updated_at === 'string');
}

/**
 * Bounded synchronous operations serialize reads and replacements on Node's
 * event loop. Publish with rename before notifying observers or replying. A
 * failed publication keeps the previous file; no in-memory cache can diverge.
 */
export class PlanStore {
    readonly dataDir: string;
    constructor(dataDir: string) { this.dataDir = dataDir; }

    private path(session: string): string {
        validateSessionId(session);
        return join(this.dataDir, 'sessions', session, 'plan.json');
    }

    read(session: string): SessionPlan {
        const path = this.path(session);
        if (!existsSync(path)) return emptyPlan();
        const fd = openSync(path, constants.O_RDONLY | constants.O_NONBLOCK);
        const buffer = Buffer.alloc(512 * 1024 + 1);
        let size = 0;
        try {
            if (!fstatSync(fd).isFile()) throw new Error('invalid plan file');
            while (size < buffer.length) {
                const count = readSync(fd, buffer, size, buffer.length - size, null);
                if (count === 0) break;
                size += count;
            }
        } finally {
            closeSync(fd);
        }
        if (size === buffer.length) throw new Error('plan file is too large');
        const bytes = buffer.subarray(0, size);
        const value: unknown = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
        if (!validPlan(value)) throw new Error('invalid plan document');
        return value;
    }

    replace(session: string, markdown: string): { plan: SessionPlan; changed: boolean } {
        if (!validMarkdown(markdown)) throw new Error('plan requires UTF-8 text of at most 64 KiB');
        if (!markdown.trim()) markdown = '';
        const previous = this.read(session);
        if (previous.markdown === markdown) return { plan: previous, changed: false };
        if (previous.revision === Number.MAX_SAFE_INTEGER) throw new Error('plan revision exhausted');
        const plan = { markdown, revision: previous.revision + 1, updated_at: new Date().toISOString() };
        const path = this.path(session);
        mkdirSync(join(this.dataDir, 'sessions', session), { recursive: true });
        const temporary = `${path}.${randomUUID()}.tmp`;
        let published = false;
        try {
            const fd = openSync(temporary, 'wx', 0o600);
            try {
                writeFileSync(fd, JSON.stringify(plan) + '\n');
                fsyncSync(fd);
            } finally {
                closeSync(fd);
            }
            renameSync(temporary, path);
            published = true;
        } finally {
            if (!published) {
                try { rmSync(temporary, { force: true }); } catch { /* preserve the original failure */ }
            }
        }
        return { plan, changed: true };
    }
}
