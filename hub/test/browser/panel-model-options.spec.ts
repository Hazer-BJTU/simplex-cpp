import { expect, test, type Page } from '@playwright/test';
import { STUB, emit, open, runningSession, withSession } from './harness.ts';

const advertised = {
    model: {
        available: [
            { name: 'model', options: ['deepseek-flash', 'deepseek-v4-pro'] },
            { name: 'reasoning_effort', options: ['low', 'high', 'max'] },
        ],
        current: { model: 'deepseek-flash', reasoning_effort: 'high' },
    },
};

async function requests(page: Page, type: string, operation?: string) {
    const response = await page.request.get(`${STUB}/__stub/received`);
    return (await response.json()).received.filter((entry: { type: string; operation?: string }) =>
        entry.type === type && (operation === undefined || entry.operation === operation));
}

test('model choices are fetched once and submitted only with a payload', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await expect.poll(async () => (await requests(page, 'signal', 'options')).length).toBe(1);
    await expect(page.getByRole('button', { name: 'Status', exact: true })).toHaveCount(0);
    await expect(page.getByRole('button', { name: 'Options', exact: true })).toHaveCount(0);
    const model = page.getByRole('button', { name: 'Model options', exact: true });
    await expect(model).toBeDisabled();
    await emit(page, 'options', advertised);
    await model.click();
    await expect(page.getByLabel('Model option: reasoning_effort')).toHaveValue('1');
    await page.getByLabel('Model option: model', { exact: true }).selectOption('1');
    await page.getByLabel('Model option: reasoning_effort').selectOption('2');
    await page.keyboard.press('Escape');
    expect((await requests(page, 'signal', 'options')).length).toBe(1);
    expect((await requests(page, 'input')).length).toBe(0);
    await page.getByLabel('message', { exact: true }).fill('hello');
    await page.getByRole('button', { name: 'Send', exact: true }).click();
    await expect.poll(async () => (await requests(page, 'input')).length).toBe(1);
    expect((await requests(page, 'input'))[0].options).toEqual({
        confirmation: { mode: 'ask' },
        model: { model: 'deepseek-v4-pro', reasoning_effort: 'max' },
    });
    await emit(page, 'run_started', {});
    await emit(page, 'run_finished', { status: 'completed' });
    await emit(page, 'input_rejected', { message: 'no turn to continue' });
    expect((await requests(page, 'signal', 'options')).length).toBe(1);
    await emit(page, 'input_rejected', { code: 'invalid_options', message: 'unsupported option' });
    await expect.poll(async () => (await requests(page, 'signal', 'options')).length).toBe(2);
    await emit(page, 'options', advertised);
    await model.click();
    await expect(page.getByLabel('Model option: model', { exact: true })).toHaveValue('0');
});

test('a replacement worker cannot reuse the previous worker model menu', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await emit(page, 'options', advertised);
    await expect(page.getByRole('button', { name: 'Model options' })).toBeEnabled();
    await withSession(page, {
        ...runningSession('demo'),
        identity: { state: 'live', worker_id: 'replacement', since: null },
    });
    await emit(page, 'ready', { active: false }, { worker_id: 'replacement' });
    await expect.poll(async () => (await requests(page, 'signal', 'options')).length).toBe(2);
    await expect(page.getByRole('button', { name: 'Model options' })).toBeDisabled();
    await emit(page, 'options', advertised, { worker_id: 'replacement' });
    await expect(page.getByRole('button', { name: 'Model options' })).toBeEnabled();
});
