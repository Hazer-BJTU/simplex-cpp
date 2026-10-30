import { expect, test, type Page } from '@playwright/test';
import { STUB, emit, open } from './harness.ts';

async function inputs(page: Page) {
    return (await (await page.request.get(`${STUB}/__stub/received`)).json()).received
        .filter((frame: { type: string }) => frame.type === 'input');
}

async function failure(page: Page) {
    await emit(page, 'run_started', {});
    await emit(page, 'run_finished', { status: 'failed', error: 'HTTP 503',
        failure: { stage: 'model_request', can_continue: true } });
}

test('failure continuation uses the current Ask/model choices and blocks every other entry', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'status', { active: false, capabilities: ['context-compact'] });
    await page.getByRole('button', { name: /confirmation mode:/ }).click();
    await page.getByRole('radio', { name: /^approve/ }).check();
    await page.keyboard.press('Escape');
    await page.getByLabel('message', { exact: true }).fill('task');
    await page.getByRole('button', { name: 'Send', exact: true }).click();
    await failure(page);
    await page.getByRole('button', { name: /confirmation mode:/ }).click();
    await page.getByRole('radio', { name: /^ask/ }).check();
    await page.keyboard.press('Escape');
    await emit(page, 'options', { model: {
        available: [{ name: 'model', options: ['first', 'second'] }], current: { model: 'first' },
    } });
    await page.getByRole('button', { name: 'Model options', exact: true }).click();
    await page.getByLabel('Model option: model', { exact: true }).selectOption('1');
    await page.keyboard.press('Escape');
    await page.request.post(`${STUB}/__stub/settings`, { data: { holdInput: true } });
    const retry = page.getByTestId('run-failure').getByRole('button', { name: 'Continue run' });
    await retry.click();
    await expect(retry).toBeDisabled();
    await retry.evaluate((button: HTMLButtonElement) => button.click());
    await page.getByLabel('message', { exact: true }).fill('unsent draft');
    await expect(page.getByRole('button', { name: 'Send', exact: true })).toBeDisabled();
    await expect(page.getByRole('button', { name: 'Compact', exact: true })).toBeDisabled();
    const sent = await inputs(page);
    expect(sent).toHaveLength(2);
    expect(sent[0].options.confirmation.mode).toBe('approve');
    expect(sent[1].operation).toBe('continue');
    expect(sent[1].options).toEqual({ confirmation: { mode: 'ask' }, model: { model: 'second' } });
    expect(sent[1]).not.toHaveProperty('content');
    await emit(page, 'input_rejected', { request_id: sent[1].request_id, message: 'fixture rejection' });
    await expect(retry).toBeEnabled();
    await expect(page.getByLabel('message', { exact: true })).toHaveValue('unsent draft');
    await page.getByRole('button', { name: 'Compact', exact: true }).click();
    await expect(retry).toBeDisabled();
    expect((await inputs(page)).map((frame: { operation: string }) => frame.operation))
        .toEqual(['message', 'continue', 'compact']);
});

for (const status of ['exchange_limit', 'cancelled', 'restored']) {
    test(`palette offers protocol continuation after ${status}, without submitting the draft`, async ({ page }) => {
        await open(page, '?session=demo');
        if (status === 'restored') {
            await page.request.post(`${STUB}/__stub/settings`, { data: { historyEnabled: true,
                historyResponses: [{ revision: 1, start: 0, step: 0, next: 1, next_step: 0, total: 1,
                    turns: [{ index: 0, user: [{ type: 'text', raw: 'Saved task', modality: 'text' }],
                        steps: [], omitted_steps: 0 }] }] } });
            await emit(page, 'status', { active: false, capabilities: ['session-history'] });
            await page.reload();
            await expect(page.getByTestId('history-turn')).toContainText('Saved task');
        } else {
            await emit(page, 'run_started', {});
            await emit(page, 'run_finished', { status });
        }
        await page.getByLabel('message', { exact: true }).fill('keep this draft');
        await page.keyboard.press('Control+k');
        await page.getByRole('textbox', { name: 'filter commands' }).fill('Continue run');
        await page.getByRole('option', { name: /Continue run/ }).click();
        await expect.poll(async () => (await inputs(page)).length).toBe(1);
        expect((await inputs(page))[0]).toMatchObject({ operation: 'continue', options: { confirmation: { mode: 'ask' } } });
        expect((await inputs(page))[0]).not.toHaveProperty('content');
        await expect(page.getByLabel('message', { exact: true })).toHaveValue('keep this draft');
    });
}
