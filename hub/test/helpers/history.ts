import type { HistoryPage, HistoryTurn } from '../../shared/protocol.ts';

/** Built-in worker limits; JSON escaping, not raw string length, fills these pages. */
export const HISTORY_PAGE_MAX_BYTES = 252 * 1024;
export const HISTORY_EVENT_MAX_BYTES = 256 * 1024;
export const HISTORY_PANEL_MAX_BYTES = 512 * 1024;

/** Mixed turn/step boundaries with near-maximum escaped display entries. */
export function boundedHistoryPages(): HistoryPage[] {
    const part = (label: string, control: string) => ({
        type: 'text', modality: 'text', raw: label + control.repeat(4096 - label.length),
    });
    const turn = (index: number): HistoryTurn => ({
        index, user: Array.from({ length: 4 }, () => part(`User ${index}`, '\u0001')),
        steps: [], omitted_steps: 0, omitted_user_parts: 0,
    });
    const withStep = (index: number, omitted: number): HistoryTurn => ({
        ...turn(2),
        steps: [{
            index,
            content: Array.from({ length: 4 }, () => part(`Answer 2.${index}`, '\u0002')),
            reasoning: part(`Reasoning 2.${index}`, '\u0003'),
            tool_calls: 0, omitted_parts: 0,
        }],
        omitted_steps: omitted,
    });
    return [
        { start: 0, step: 0, next: 2, next_step: 0, turns: [turn(0), turn(1)] },
        { start: 2, step: 0, next: 2, next_step: 1, turns: [withStep(0, 1)] },
        { start: 2, step: 1, next: 3, next_step: 0, turns: [withStep(1, 0)] },
        { start: 3, step: 0, next: 4, next_step: 0, turns: [turn(3)] },
    ].map((page, index) => ({ ...page, request_id: `bounded-${index}`, revision: 7, total: 4 }));
}
