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
 * (including admission/replay ordering), then stabilize presentation separately.
 * A surviving source event/outbox keeps the execution's DOM key even after its
 * admission or input is removed. Wire IDs alone cannot establish continuity:
 * reused IDs without retained evidence receive a new key. Only the latest
 * rounds and their retained source-to-key map are kept; parsing caches are weak.
 */
export function createRoundProjection() {
    const derivations = new RoundDerivations();
    let previous: readonly Round[] = [];
    let retainedKeys = new Map<string, string>();
    let generation = 0;
    return (items: readonly TranscriptItem[], prompts: ReadonlyMap<string, ConfirmationPrompt>,
        requests: ReadonlyMap<string, RequestRecord>): readonly Round[] => {
        const fresh = buildRounds(items, prompts, requests, derivations);
        const byKey = new Map(previous.map(round => [round.key, round]));
        const used = new Set<string>();
        const nextKeys = new Map<string, string>();
        const next = fresh.map(round => {
            const sources = round.sourceKeys;
            let inherited: string | undefined;
            for (const source of sources) {
                const previousKey = retainedKeys.get(source);
                if (previousKey !== undefined && !used.has(previousKey)) {
                    inherited = previousKey;
                    break;
                }
            }
            let key = inherited ?? round.key;
            // A split cannot assign one DOM key to two rounds. Nor may a new
            // execution inherit an old key solely because wire IDs were reused.
            if (used.has(key) || (inherited === undefined && byKey.has(key))) {
                key = JSON.stringify(['presentation', round.key, ++generation]);
            }
            used.add(key);
            for (const source of sources) nextKeys.set(source, key);
            const stable = key === round.key ? round : { ...round, key };
            return share(byKey.get(key), stable) as Round;
        });
        retainedKeys = nextKeys;
        const unchanged = next.length === previous.length
            && next.every((round, index) => round === previous[index]);
        if (!unchanged) previous = next;
        return previous;
    };
}
