/** Validate a worker history page before it changes state or advances a cursor. */
import type { ContentPart, HistoryPage } from '../../../shared/protocol.ts';

function record(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function index(value: unknown): value is number {
    return Number.isSafeInteger(value) && (value as number) >= 0;
}

function part(value: unknown): value is ContentPart {
    return record(value) && typeof value.type === 'string'
        && typeof value.raw === 'string';
}

function parts(value: unknown): value is ContentPart[] {
    return Array.isArray(value) && value.length <= 4 && value.every(part);
}

/** Return null for malformed structure or a cursor inconsistent with its page. */
export function parseHistoryPage(value: unknown): HistoryPage | null {
    if (!record(value) || typeof value.request_id !== 'string'
        || value.request_id.length === 0 || !index(value.revision)) return null;
    for (const key of ['start', 'step', 'next', 'next_step', 'total']) {
        if (!index(value[key])) return null;
    }
    if (!Array.isArray(value.turns) || value.turns.length > 10
        || (value.next as number) > (value.total as number)
        || (value.start as number) > (value.total as number)
        || (value.start as number) + value.turns.length > (value.total as number)) return null;

    for (let turnOffset = 0; turnOffset < value.turns.length; turnOffset += 1) {
        const turn = value.turns[turnOffset];
        if (!record(turn) || !index(turn.index)
            || turn.index !== (value.start as number) + turnOffset
            || !parts(turn.user) || !Array.isArray(turn.steps)
            || !index(turn.omitted_steps)
            || turn.omitted_user_parts !== undefined && !index(turn.omitted_user_parts)) return null;
        let expectedStep = turnOffset === 0 ? value.step as number : 0;
        for (const step of turn.steps) {
            if (!record(step) || !index(step.index) || step.index !== expectedStep
                || !parts(step.content) || !index(step.tool_calls)
                || step.omitted_parts !== undefined && !index(step.omitted_parts)
                || step.reasoning !== undefined && !part(step.reasoning)) return null;
            expectedStep += 1;
        }
        if (turnOffset < value.turns.length - 1 && turn.omitted_steps !== 0) return null;
    }

    if (value.next_step === 0) {
        if (value.next !== (value.start as number) + value.turns.length
            || value.turns.some((turn) => turn.omitted_steps !== 0)) return null;
    } else {
        const last = value.turns.at(-1);
        const firstStep = value.turns.length === 1 ? value.step as number : 0;
        if (!last || value.next !== (value.start as number) + value.turns.length - 1
            || last.omitted_steps === 0
            || value.next_step !== firstStep + last.steps.length
            || (value.next_step as number) <= firstStep) return null;
    }
    return value as unknown as HistoryPage;
}
