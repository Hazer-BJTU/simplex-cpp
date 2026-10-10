/** Aggregate display accounting and open approvals, indexed by immutable view maps. */
import type { ConfirmationPrompt, SessionId } from '../../../shared/protocol.ts';
import { profileCount } from '../lib/profile.ts';
import { viewDisplayBytes, type ViewState } from './view.ts';

type Views = ReadonlyMap<SessionId, ViewState>;
export interface ViewProjection {
    readonly bytes: number;
    readonly prompts: readonly ConfirmationPrompt[];
}

// Weak keys do not extend the lifetime of replaced store snapshots or their data.
const projections = new WeakMap<Views, ViewProjection>();

/** Rebuild only when confirmation maps change; stable sort preserves view/prompt order on ties. */
function openPrompts(views: Views): readonly ConfirmationPrompt[] {
    profileCount('approvalProjection');
    const open: ConfirmationPrompt[] = [];
    for (const view of views.values()) {
        for (const prompt of view.confirmations.values()) {
            if (prompt.settled_at === null) open.push(prompt);
        }
    }
    return open.sort((a, b) => a.received_at.localeCompare(b.received_at));
}

/**
 * Normal event updates seed this cache with deltas. Scan only a previously
 * unindexed map, including a snapshot supplied through the vanilla store API.
 * No projection mutates views, approval authority, or existing cache entries.
 */
export function viewProjection(views: Views): ViewProjection {
    const cached = projections.get(views);
    if (cached) return cached;
    profileCount('viewProjectionScan');
    let bytes = 0;
    for (const view of views.values()) bytes += viewDisplayBytes(view);
    const projection = { bytes, prompts: openPrompts(views) };
    projections.set(views, projection);
    return projection;
}

/**
 * Index a replacement map before notifying subscribers. The caller supplies
 * every changed/removed session once, including budget evictions. Ordinary
 * events account for only their session and reuse the exact approval array;
 * actual prompt updates publish immediately, without a frame/timer delay.
 */
export function indexViewChanges(previous: Views, next: Views, changed: Iterable<SessionId>): void {
    const cached = viewProjection(previous);
    let bytes = cached.bytes;
    let promptsChanged = false;
    for (const id of changed) {
        const before = previous.get(id);
        const after = next.get(id);
        bytes += (after ? viewDisplayBytes(after) : 0) - (before ? viewDisplayBytes(before) : 0);
        if (before?.confirmations !== after?.confirmations
            && ((before?.confirmations.size ?? 0) > 0 || (after?.confirmations.size ?? 0) > 0)) {
            promptsChanged = true;
        }
    }
    let prompts = cached.prompts;
    if (promptsChanged) {
        const open = openPrompts(next);
        if (open.length !== prompts.length || open.some((prompt, index) => prompt !== prompts[index])) {
            prompts = open;
        }
    }
    profileCount('viewProjectionDelta');
    projections.set(next, { bytes, prompts });
}
