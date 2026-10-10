/** Join bounded history to replay by durable execution and response identities. */
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
function roundIdentities(round: Round, baseline: number, worker: string): string[] {
    const envelopes = [round.admitted?.envelope, ...round.assistant.map(block => block.envelope),
        ...round.protocol.map(item => item.envelope)];
    return envelopes.flatMap(envelope => envelope?.worker_id && envelope.request_id && envelope.run_id
        && typeof envelope.sequence === 'number'
        && (envelope.worker_id !== worker || envelope.sequence <= baseline)
        ? [identity(envelope as ExecutionIdentity)] : []);
}

/** Duplicate replay executions provide no unique owner; never let the last win. */
function executionOwners(runs: readonly Round[], baseline: number, worker: string): Map<string, number | null> {
    const owners = new Map<string, number | null>();
    runs.forEach((round, index) => {
        for (const execution of new Set(roundIdentities(round, baseline, worker))) {
            owners.set(execution, owners.has(execution) ? null : index);
        }
    });
    return owners;
}

/**
 * Insert missing history responses into their execution's timeline. Replayed
 * responses and tool cards remain intact. Commit order places a recovered step
 * before the next response; a final step precedes run_finished when available.
 * Unmatched executions remain standalone history rather than being guessed from
 * a turn index, input source, or sequence shared by another worker incarnation.
 */
function restoreResponses(
    history: readonly HistoryTurn[], runs: readonly Round[], baseline: number, worker: string,
    ordinary = false,
): { history: HistoryTurn[]; rounds: Round[] } {
    const owners = executionOwners(runs, baseline, worker);
    const rounds = [...runs];
    const unmatched: HistoryTurn[] = [];
    const covered = coveredResponses(runs, baseline, worker);
    const uncovered = ordinary ? history.map(turn => ({ ...turn, steps: turn.steps.filter(step =>
        !step.execution || !commitIdentity(step.commit_sequence)
        || !covered.has(responseIdentity(step.execution, step.commit_sequence))) }))
        : uncoveredInternalHistory(history, runs, baseline, worker);
    for (const turn of uncovered) {
        const steps: HistoryTurn['steps'] = [];
        for (const step of turn.steps) {
            // Only persisted per-response identity authorizes placement. Legacy
            // input provenance cannot safely assign a resumed execution.
            const owner = step.execution && commitIdentity(step.commit_sequence)
                ? owners.get(identity(step.execution)) : undefined;
            if (owner === undefined || owner === null) {
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
        if (ordinary || steps.length || turn.omitted_steps) unmatched.push({ ...turn, steps });
    }
    return { history: unmatched, rounds };
}

/** Compatibility helper for private continuation projections. */
export function reconcileInternalHistory(
    history: readonly HistoryTurn[], runs: readonly Round[], baseline: number, worker: string,
): { history: HistoryTurn[]; rounds: Round[] } {
    return restoreResponses(history, runs, baseline, worker);
}

/**
 * Input identity and response identity are separate. A Continue execution can
 * restore responses into its own round but cannot inherit the original user's
 * bubble. Inputs lacking provenance remain standalone, even when their replies
 * can be matched. Turn indices are display cursors, never correlation keys.
 * Responses with no unique execution/commit match remain standalone too.
 */
export function reconcileHistory(
    history: readonly HistoryTurn[], runs: readonly Round[], baseline: number, worker: string,
): { olderHistory: HistoryTurn[]; historyForRun: Map<string, HistoryTurn>;
    restoredRuns: Round[]; linkedInputs: Set<number> } {
    const owners = executionOwners(runs, baseline, worker);
    const sources = new Map<string, number>();
    for (const turn of history) {
        if (turn.source && !turn.internal_input) {
            const key = identity(turn.source);
            sources.set(key, (sources.get(key) ?? 0) + 1);
        }
    }
    const mapped = new Map<string, HistoryTurn>();
    const linkedInputs = new Set<number>();
    for (const turn of history) {
        if (!turn.source || turn.internal_input) continue;
        const key = identity(turn.source);
        const owner = sources.get(key) === 1 ? owners.get(key) : undefined;
        if (owner === undefined || owner === null) continue;
        const round = runs[owner]!;
        if (round.continued || round.compacting) continue;
        mapped.set(round.key, turn);
        linkedInputs.add(turn.index);
    }
    const restored = restoreResponses(history, runs, baseline, worker, true);
    return {
        olderHistory: restored.history.filter(turn => turn.steps.length > 0 || turn.omitted_steps > 0
            || !turn.internal_input && !linkedInputs.has(turn.index)),
        historyForRun: mapped,
        restoredRuns: restored.rounds,
        linkedInputs,
    };
}
