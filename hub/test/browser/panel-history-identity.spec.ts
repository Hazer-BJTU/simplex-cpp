import { expect, test, type Page } from '@playwright/test';
import type { ExecutionIdentity, HistoryTurn } from '../../shared/protocol.ts';
import { STUB, call, emit, modelResponse, open, toolResult } from './harness.ts';

const A = { worker_id: 'stub-worker', request_id: 'A', run_id: 'run-A' };
const B = { worker_id: 'stub-worker', request_id: 'B', run_id: 'run-B' };
const part = (raw: string) => ({ type: 'text', modality: 'text', raw });
const step = (index: number, execution: ExecutionIdentity, raw: string, commit: string) => ({
    index, execution, commit_sequence: commit, content: [part(raw)], tool_calls: 0,
});
const turn = (user = 'Original input A'): HistoryTurn => ({ index: 0, source: A,
    user: [part(user)], steps: [step(0, A, 'Answer A', '1')], omitted_steps: 0 });

async function prepare(page: Page, history: HistoryTurn[]) {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: { historyEnabled: true, historyTurns: history } });
    await emit(page, 'ready', { active: false, capabilities: ['session-history'] }, { request_id: '', run_id: '' });
}

async function replayA(page: Page) {
    await emit(page, 'input_admitted', { operation: 'message' }, A);
    await emit(page, 'run_started', {}, A);
    await emit(page, 'input_committed', {}, A);
    await emit(page, 'model_response', modelResponse('Answer A', { commit_sequence: '1',
        invokes: [call('tool-A', 'run_command', { command: 'echo A' })] }), A);
    await emit(page, 'tool_results', [toolResult('tool-A', 'run_command', 'Output A')], A);
    await emit(page, 'run_finished', { status: 'completed' }, A);
}

async function refresh(page: Page) {
    await page.getByLabel('message').press('Alt+Enter');
    await page.getByLabel('command input').fill('Refresh');
    await page.getByLabel('command input').press('Enter');
}

async function queries(page: Page): Promise<{ request_id: string; start: number }[]> {
    const response = await page.request.get(`${STUB}/__stub/received`);
    return (await response.json()).received.filter((message: { type: string }) => message.type === 'history');
}

test('new admission cannot steal an older history input during refresh or reload', async ({ page }) => {
    await prepare(page, [turn()]);
    await replayA(page);
    await page.goto('/?session=demo');
    await expect(page.getByTestId('restored-user-message')).toContainText('Original input A');
    await emit(page, 'input_admitted', { operation: 'message' }, B);
    await emit(page, 'input_committed', {}, B);
    await refresh(page);
    await expect.poll(async () => (await queries(page)).length).toBe(2);
    for (const reloaded of [false, true]) {
        if (reloaded) await page.reload();
        const rounds = page.getByTestId('round').filter({ has: page.getByTestId('round-summary') });
        await expect(rounds).toHaveCount(2);
        await expect(rounds.nth(0).getByTestId('restored-user-message')).toContainText('Original input A');
        await expect(rounds.nth(1).getByTestId('restored-user-message')).toHaveCount(0);
        await expect(rounds.nth(1).getByTestId('admitted-placeholder')).toBeVisible();
        await expect(page.getByTestId('history-turn')).toHaveCount(0);
    }
});

test('Continue restores a missing answer beside its own run without duplicating input or tools', async ({ page }) => {
    const history = turn();
    history.steps.push(step(1, B, 'Continued answer B', '2'));
    await prepare(page, [history]);
    await replayA(page);
    await emit(page, 'input_admitted', { operation: 'continue' }, B);
    await emit(page, 'run_started', {}, B);
    await emit(page, 'run_finished', { status: 'completed' }, B);
    await page.goto('/?session=demo');
    for (const reloaded of [false, true]) {
        if (reloaded) await page.reload();
        const rounds = page.getByTestId('round').filter({ has: page.getByTestId('round-summary') });
        await expect(rounds).toHaveCount(2);
        await expect(rounds.nth(0).getByTestId('restored-user-message')).toContainText('Original input A');
        await expect(rounds.nth(1).getByTestId('assistant-message')).toContainText('Continued answer B');
        await expect(rounds.nth(1).getByTestId('restored-user-message')).toHaveCount(0);
        await expect(rounds.nth(1).getByTestId('admitted-placeholder')).toHaveCount(0);
        await expect(page.getByTestId('history-turn')).toHaveCount(0);
        await expect(page.getByTestId('tool-card')).toHaveCount(1);
        await rounds.nth(0).getByTestId('tool-details').locator('summary').first().click();
        await expect(rounds.nth(0)).toContainText('Output A');
    }
});

test('legacy user text remains separate with an explanation instead of a positional match', async ({ page }) => {
    const legacy = turn('Legacy user input');
    delete legacy.source;
    await prepare(page, [legacy]);
    await replayA(page);
    await page.goto('/?session=demo');
    await expect(page.getByTestId('history-unmatched-notice')).toBeVisible();
    await expect(page.getByTestId('history-turn')).toContainText('Legacy user input');
    await expect(page.getByTestId('restored-user-message')).toHaveCount(0);
    await expect(page.getByTestId('assistant-message')).toHaveCount(1);
    await expect(page.getByTestId('admitted-placeholder')).toBeVisible();
});

test('revision drift and invalid retry preserve the previously displayed history', async ({ page }) => {
    await prepare(page, [turn()]);
    await replayA(page);
    await page.goto('/?session=demo');
    await expect(page.getByTestId('restored-user-message')).toContainText('Original input A');
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyResponses: [{ __hold: true }, { __hold: true }, { __hold: true }],
    } });
    await refresh(page);
    await expect.poll(async () => (await queries(page)).length).toBe(2);
    const first = (await queries(page)).at(-1)!;
    await emit(page, 'history', { request_id: first.request_id, revision: 3,
        start: 0, step: 0, next: 1, next_step: 0, total: 2, turns: [turn('UNVALIDATED REPLACEMENT')] });
    await expect.poll(async () => (await queries(page)).length).toBe(3);
    await expect(page.getByTestId('transcript')).not.toContainText('UNVALIDATED REPLACEMENT');
    await emit(page, 'input_admitted', { operation: 'message' }, B);
    await emit(page, 'input_committed', {}, B);
    const second = (await queries(page)).at(-1)!;
    await emit(page, 'history', { request_id: second.request_id, revision: 4,
        start: 1, step: 0, next: 2, next_step: 0, total: 2,
        turns: [{ ...turn('Input B'), index: 1, source: B, steps: [] }] });
    await expect.poll(async () => (await queries(page)).length).toBe(4);
    const retry = (await queries(page)).at(-1)!;
    await emit(page, 'history', { request_id: retry.request_id, revision: null });
    await expect(page.getByText('Worker returned an invalid conversation history page.')).toBeVisible();
    await expect(page.getByTestId('restored-user-message')).toContainText('Original input A');
    await expect(page.getByTestId('transcript')).not.toContainText('UNVALIDATED REPLACEMENT');
    await expect(page.getByTestId('transcript')).not.toContainText('loading conversation history');
    await expect(page.getByTestId('restored-user-message')).toHaveCount(1);
});

test('an empty unanswered input is restored without a model response', async ({ page }) => {
    await prepare(page, [{ ...turn(''), steps: [] }]);
    await emit(page, 'input_admitted', { operation: 'message' }, A);
    await emit(page, 'input_committed', {}, A);
    await emit(page, 'run_finished', { status: 'cancelled' }, A);
    await page.goto('/?session=demo');
    await expect(page.getByTestId('restored-user-message')).toContainText('(empty input)');
    await expect(page.getByTestId('history-turn')).toHaveCount(0);
    await expect(page.getByTestId('admitted-placeholder')).toHaveCount(0);
});
