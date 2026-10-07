/** Reconcile private continuation responses without changing input provenance. */
import type { ExecutionIdentity, HistoryTurn, WorkerEnvelope } from '../../../shared/protocol.ts';
import { contentText } from './content.ts';
import type { AssistantBlock, Round } from './rounds.ts';

function identity(value: ExecutionIdentity): string {
    return JSON.stringify([value.worker_id, value.request_id, value.run_id]);
}

function commitIdentity(value: unknown): value is string {
    return typeof value === 'string' && /^[1-9][0-9]{0,19}$/.test(value);
}

function responseIdentity(execution: ExecutionIdentity, commit: string): string {
    return JSON.stringify([identity(execution), commit]);
}

/** The event cursor bounds the queried incarnation, not earlier incarnations. */
function coveredResponses(runs: readonly Round[], baseline: number, worker?: string): Set<string> {
    return new Set(runs.flatMap((round) => round.assistant.flatMap(({ envelope }) => {
        if (typeof envelope.sequence !== 'number'
            || (!worker || envelope.worker_id === worker) && envelope.sequence > baseline
            || !envelope.worker_id || !envelope.request_id || !envelope.run_id) return [];
        const data = envelope.data as Record<string, unknown> | null;
        return commitIdentity(data?.commit_sequence)
            ? [responseIdentity(envelope as ExecutionIdentity, data.commit_sequence)] : [];
    })));
}

/**
 * A turn's source identifies its private input, not its later Continue replies.
 * Legacy steps without execution metadata retain the conservative source match;
 * new snapshots identify every response independently, including after restart.
 */
export function uncoveredInternalHistory(
    history: readonly HistoryTurn[], runs: readonly Round[], baseline: number, worker?: string,
): HistoryTurn[] {
    const covered = coveredResponses(runs, baseline, worker);
    return history.flatMap((turn) => {
        const steps = turn.steps.filter((step) => {
            const execution = step.execution ?? turn.source;
            return !execution || !step.commit_sequence
                || !covered.has(responseIdentity(execution, step.commit_sequence));
        });
        return steps.length || turn.omitted_steps ? [{ ...turn, steps }] : [];
    });
}

/** Find a replayed execution even when its model response has been evicted. */
function roundIdentities(round: Round): string[] {
    const envelopes = [round.admitted?.envelope, ...round.assistant.map(block => block.envelope),
        ...round.protocol.map(item => item.envelope)];
    return envelopes.flatMap(envelope => envelope?.worker_id && envelope.request_id && envelope.run_id
        ? [identity(envelope as ExecutionIdentity)] : []);
}

/**
 * Insert missing history responses into their execution's timeline. Replayed
 * responses and tool cards remain intact. Commit order places a recovered step
 * before the next response; a final step precedes run_finished when available.
 * Unmatched executions remain standalone history rather than being guessed from
 * a turn index, input source, or sequence shared by another worker incarnation.
 */
export function reconcileInternalHistory(
    history: readonly HistoryTurn[], runs: readonly Round[], baseline: number, worker: string,
): { history: HistoryTurn[]; rounds: Round[] } {
    const owners = new Map<string, number>();
    runs.forEach((round, index) => {
        for (const execution of roundIdentities(round)) owners.set(execution, index);
    });
    const rounds = [...runs];
    const unmatched: HistoryTurn[] = [];
    for (const turn of uncoveredInternalHistory(history, runs, baseline, worker)) {
        const steps: HistoryTurn['steps'] = [];
        for (const step of turn.steps) {
            // Only persisted per-response identity authorizes placement. Legacy
            // input provenance cannot safely assign a resumed execution.
            const owner = step.execution ? owners.get(identity(step.execution)) : undefined;
            if (owner === undefined) {
                steps.push(step);
                continue;
            }
            const round = rounds[owner]!;
            const key = `history:${turn.index}:${step.index}`;
            const block: AssistantBlock = {
                key, envelope: { ...step.execution, event: 'model_response',
                    data: { commit_sequence: step.commit_sequence } } as WorkerEnvelope,
                text: step.content.map(contentText).filter(Boolean).join('\n\n'),
                reasoning: step.reasoning ? contentText(step.reasoning) : '',
                cost: '', tokens: null, clock: '', callIds: [], historyStep: step,
            };
            const assistant = [...round.assistant];
            const next = step.commit_sequence ? assistant.findIndex(candidate => {
                const commit = (candidate.envelope.data as Record<string, unknown> | null)?.commit_sequence;
                return commitIdentity(commit) && BigInt(commit) > BigInt(step.commit_sequence!);
            }) : -1;
            const timeline = [...round.timeline];
            let before = next >= 0 ? timeline.findIndex(entry => entry.key === assistant[next]!.key)
                : timeline.findIndex(entry => entry.kind === 'protocol'
                    && round.protocol.some(item => item.id === entry.key && item.envelope.event === 'run_finished'));
            if (before < 0) before = timeline.length;
            timeline.splice(before, 0, { kind: 'assistant', key });
            assistant.splice(next >= 0 ? next : assistant.length, 0, block);
            rounds[owner] = { ...round, assistant, timeline };
        }
        if (steps.length || turn.omitted_steps) unmatched.push({ ...turn, steps });
    }
    return { history: unmatched, rounds };
}
