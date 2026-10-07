/** Match private continuation history by host identity, never by array position. */
import type { HistoryTurn } from '../../../shared/protocol.ts';
import type { Round } from './rounds.ts';

/**
 * Keep responses missing from replay while retaining live tool cards. Comparing
 * committed response identities also handles a replay that starts mid-segment,
 * repeated compactions within one run, and a restored worker incarnation.
 */
export function uncoveredInternalHistory(
    history: readonly HistoryTurn[], runs: readonly Round[], baseline: number,
): HistoryTurn[] {
    return history.flatMap((turn) => {
        const source = turn.source;
        if (!source) return [turn];
        const covered = new Set(runs.flatMap((round) => round.assistant.flatMap(({ envelope }) => {
            if (envelope.worker_id !== source.worker_id || envelope.request_id !== source.request_id
                || envelope.run_id !== source.run_id || typeof envelope.sequence !== 'number'
                || envelope.sequence > baseline) return [];
            const data = envelope.data as Record<string, unknown> | null;
            return typeof data?.commit_sequence === 'string' ? [data.commit_sequence] : [];
        })));
        const steps = turn.steps.filter((step) => !step.commit_sequence || !covered.has(step.commit_sequence));
        return steps.length || turn.omitted_steps ? [{ ...turn, steps }] : [];
    });
}
