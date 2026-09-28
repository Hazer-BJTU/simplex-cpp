import { expect, test } from '@playwright/test';
import { STUB, emit, open, runningSession, withSession } from './harness.ts';

const result = { summary: 'A **saved summary**', memory_file: '/worker/memory/archive/state.md',
    removed_turns: 1, revision: 2, durable: true };

test('Compact context is gated, carries no message, and preserves a refused draft', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await page.getByLabel('message').fill('Keep this draft');
    await page.getByLabel('message').press('Alt+Enter');
    await page.getByLabel('command input').fill('comp');
    const command = page.getByRole('option', { name: /Compact context/ });
    await expect(command).toBeDisabled();
    await emit(page, 'status', { active: false, capabilities: ['context-compact'],
        memory_retention: { max_archives: 5 } });
    await expect(command).toBeEnabled();
    await expect(command).toContainText('5 archives');
    await page.getByLabel('command input').press('Enter');
    await expect(page.getByTestId('round-summary').last()).toContainText('context compaction');
    await expect(page.getByTestId('outbox-item')).toHaveCount(0);
    await expect(page.getByTestId('admitted-placeholder')).toHaveCount(0);
    await emit(page, 'compact_finished', { ...result, archive_cleanup_error: 'permission denied' });
    await emit(page, 'run_finished', { status: 'completed' });
    await expect(page.getByTestId('compact-result')).toContainText('saved summary');
    await expect(page.getByTestId('compact-result')).toContainText('archive cleanup failed');
    await expect(page.getByTestId('compact-result').locator('strong')).toHaveText('saved summary');
    await page.request.post(`${STUB}/__stub/settings`, { data: { rejectInput: true } });
    await page.getByLabel('command input').fill('comp');
    await page.getByLabel('command input').press('Enter');
    await expect(page.getByTestId('transcript-problem')).toContainText('no turns to compact');
    await expect(page.getByText('Context compaction was rejected.', { exact: false })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Cancel run', exact: true })).toHaveCount(0);
    await page.request.post(`${STUB}/__stub/settings`, { data: { refuseInput: true } });
    await page.getByLabel('command input').fill('comp');
    await page.getByLabel('command input').press('Enter');
    await expect(page.getByText('the stub refused the input')).toBeVisible();
    await page.getByLabel('command input').press('Alt+Enter');
    await expect(page.getByRole('textbox', { name: 'message', exact: true })).toHaveValue('Keep this draft');
    const response = await page.request.get(`${STUB}/__stub/received`);
    const inputs = (await response.json()).received.filter((message: { type: string }) => message.type === 'input');
    expect(inputs).toHaveLength(3);
    for (const input of inputs) {
        expect(input.operation).toBe('compact');
        expect(input).not.toHaveProperty('content');
    }
    await page.reload();
    await expect(page.getByTestId('compact-result')).toContainText('saved summary');
    await expect(page.getByTestId('admitted-placeholder')).toHaveCount(0);
});

test('a rejected compact retains its identity after transcript replay', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await emit(page, 'status', { active: false, capabilities: ['context-compact'] });
    await page.request.post(`${STUB}/__stub/settings`, { data: { rejectInput: true } });
    await page.getByLabel('message').press('Alt+Enter');
    await page.getByLabel('command input').fill('compact');
    await page.getByLabel('command input').press('Enter');
    await expect(page.getByText('Context compaction was rejected.', { exact: false })).toBeVisible();
    // The real hub retains the request record in its session description.
    // The stub deliberately does not, so provide the same replay input here.
    const response = await page.request.get(`${STUB}/__stub/received`);
    const input = (await response.json()).received.find((entry: { type: string }) =>
        entry.type === 'input');
    await withSession(page, {
        ...runningSession('demo'),
        requests: [{ request_id: input.request_id, operation: 'compact', state: 'rejected',
            sent_at: new Date().toISOString(), settled_at: new Date().toISOString(),
            detail: 'no turns to compact' }],
    });
    await page.reload();
    await expect(page.getByText('Context compaction was rejected.', { exact: false })).toBeVisible();
    await expect(page.getByTestId('round-summary').last()).toContainText('context compaction');
    // New worker events carry the operation themselves, so this remains
    // identifiable after the hub's shorter request-record window expires.
    await withSession(page, runningSession('demo'));
    await page.reload();
    await expect(page.getByText('Context compaction was rejected.', { exact: false })).toBeVisible();
});

test('successful compaction retires old history queries and refreshes the new revision', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: { historyEnabled: true,
        historyResponses: [{ __hold: true }, { __hold: true }] } });
    await emit(page, 'status', { active: false, capabilities: ['context-compact', 'session-history'] });
    await page.goto('/?session=demo');
    const queries = async () => {
        const response = await page.request.get(`${STUB}/__stub/received`);
        return (await response.json()).received.filter((message: { type: string }) => message.type === 'history');
    };
    await expect.poll(async () => (await queries()).length).toBe(1);
    const old = (await queries())[0];
    await emit(page, 'input_admitted', { operation: 'compact' });
    await emit(page, 'compact_finished', result);
    await emit(page, 'run_finished', { status: 'completed' });
    await expect.poll(async () => (await queries()).length).toBe(2);
    const fresh = (await queries())[1];
    expect(fresh.request_id).not.toBe(old.request_id);
    await emit(page, 'history', { request_id: old.request_id, revision: 1,
        start: 0, step: 0, next: 1, next_step: 0, total: 1,
        turns: [{ index: 0, user: [{ type: 'text', raw: 'Stale archived user text', modality: 'text' }],
            steps: [], omitted_steps: 0 }] });
    await expect(page.getByTestId('history-turn')).toHaveCount(0);
    await emit(page, 'history', { request_id: fresh.request_id, revision: 2,
        start: 0, step: 0, next: 0, next_step: 0, total: 0, turns: [] });
    await expect(page.getByText('loading conversation history…')).toHaveCount(0);
    await expect(page.getByTestId('compact-result')).toContainText('saved summary');
});

test('failed or cancelled compact preserves history and uses compact-specific guidance', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: { historyEnabled: true,
        historyTurns: [{ index: 0, user: [{ type: 'text', raw: 'Original conversation', modality: 'text' }],
            steps: [], omitted_steps: 0 }] } });
    await emit(page, 'status', { active: false, capabilities: ['context-compact', 'session-history'] });
    await page.goto('/?session=demo');
    await expect(page.getByTestId('history-turn')).toContainText('Original conversation');
    await emit(page, 'input_admitted', { operation: 'compact' });
    await emit(page, 'run_finished', { status: 'failed', error: 'provider failed',
        failure: { stage: 'model_request', can_continue: true } });
    await expect(page.getByTestId('run-failure')).toContainText('Context compaction failed');
    await expect(page.getByTestId('run-failure')).not.toContainText('Continue run');
    await emit(page, 'input_admitted', { operation: 'compact' }, { request_id: 'retry-compact' });
    await page.getByLabel('message').press('Alt+Enter');
    await page.getByRole('button', { name: 'Cancel run', exact: true }).click();
    const received = await page.request.get(`${STUB}/__stub/received`);
    expect((await received.json()).received.some((message: { type: string; operation?: string }) =>
        message.type === 'signal' && message.operation === 'cancel')).toBe(true);
    await emit(page, 'run_finished', { status: 'cancelled' }, { request_id: 'retry-compact' });
    await expect(page.getByText('Context compaction cancelled.', { exact: false })).toBeVisible();
    await expect(page.getByTestId('history-turn')).toContainText('Original conversation');
});
