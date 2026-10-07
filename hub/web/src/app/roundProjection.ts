import type { ConfirmationPrompt, RequestRecord } from '../../../shared/protocol.ts';
import type { TranscriptItem } from '../state/view.ts';
import { buildRounds, RoundDerivations, type Round } from './rounds.ts';

/**
 * Share generated presentation records; wire objects are immutable leaves.
 * A depth bound avoids walking arbitrary deeply nested tool arguments. Reusing
 * is only an optimization: a changed/deep value always keeps the fresh result.
 */
function share(previous: unknown, next: unknown, depth = 0): unknown {
    if (Object.is(previous, next)) return previous;
    if (depth >= 8 || !previous || !next || typeof previous !== 'object'
        || typeof next !== 'object' || Array.isArray(previous) !== Array.isArray(next)) return next;
    if (!Array.isArray(next) && (Object.getPrototypeOf(next) !== Object.prototype
        || Object.getPrototypeOf(previous) !== Object.prototype)) return next;
    const before = previous as Record<string, unknown>;
    const after = next as Record<string, unknown>;
    const keys = Object.keys(after);
    if (keys.length !== Object.keys(before).length) return next;
    let equal = true;
    const result: Record<string, unknown> = Array.isArray(next) ? [] as unknown as Record<string, unknown> : {};
    for (const key of keys) {
        if (!Object.hasOwn(before, key)) return next;
        if (key === '__proto__') return next;
        // Preserve wire identity rather than traversing arbitrary user JSON.
        result[key] = ['args', 'envelope', 'prompt', 'input', 'admitted'].includes(key)
            ? after[key] : share(before[key], after[key], depth + 1);
        if (result[key] !== before[key]) equal = false;
    }
    return equal ? previous : result;
}

/**
 * One visible transcript's projection. Always run the full correctness fold
 * (including admission/replay ordering), reuse immutable parsing and unchanged
 * records afterwards. Only the latest rounds are strongly retained; weak parse
 * caches expire with their source events. No cross-session or string-key cache.
 */
export function createRoundProjection() {
    const derivations = new RoundDerivations();
    let previous: readonly Round[] = [];
    return (items: readonly TranscriptItem[], prompts: ReadonlyMap<string, ConfirmationPrompt>,
        requests: ReadonlyMap<string, RequestRecord>): readonly Round[] => {
        const fresh = buildRounds(items, prompts, requests, derivations);
        const byKey = new Map(previous.map(round => [round.key, round]));
        const next = fresh.map(round => share(byKey.get(round.key), round) as Round);
        const unchanged = next.length === previous.length
            && next.every((round, index) => round === previous[index]);
        if (!unchanged) previous = next;
        return previous;
    };
}
