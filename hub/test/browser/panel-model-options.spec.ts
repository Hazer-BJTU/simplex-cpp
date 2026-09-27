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

test('an unanswered options request is retried after the panel reconnects', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await expect.poll(async () => (await requests(page, 'signal', 'options')).length).toBe(1);
    await page.request.post(`${STUB}/__stub/down`);
    await expect(page.getByText('connected', { exact: true })).toHaveCount(0);
    await page.request.post(`${STUB}/__stub/up`);
    await expect(page.getByText('connected', { exact: true })).toBeVisible();
    await expect.poll(async () => (await requests(page, 'signal', 'options')).length).toBe(2);
    await emit(page, 'options', advertised);
    await expect(page.getByRole('button', { name: 'Model options' })).toBeEnabled();
});

test('an unanswered options request is retried when only the worker reconnects', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await expect.poll(async () => (await requests(page, 'signal', 'options')).length).toBe(1);
    await page.request.post(`${STUB}/__stub/connection`, { data: { connected: false } });
    await page.request.post(`${STUB}/__stub/connection`, { data: { connected: true } });
    await expect.poll(async () => (await requests(page, 'signal', 'options')).length).toBe(2);
    await emit(page, 'options', advertised);
    await expect(page.getByRole('button', { name: 'Model options' })).toBeEnabled();
});

test('mode switching keeps the same toolbar and disables message settings', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await emit(page, 'options', advertised);
    const confirm = page.getByRole('button', { name: /^confirmation mode:/ });
    const model = page.getByRole('button', { name: 'Model options' });
    const send = page.getByRole('button', { name: 'Send', exact: true });
    const controls = [page.getByLabel('Attach a reference'), confirm, model, send];
    const before = await Promise.all(controls.map((control) => control.boundingBox()));
    const messageColor = await page.getByTestId('composer-mode').evaluate(
        (node) => getComputedStyle(node).backgroundColor);
    await send.evaluate((node) => { node.dataset.sharedAction = 'yes'; });
    await model.click();
    await expect(page.getByLabel('Model option: model', { exact: true })).toBeVisible();
    await page.keyboard.press('Escape');
    await page.getByLabel('message', { exact: true }).press('Alt+Enter');
    await expect(page.getByLabel('Model option: model', { exact: true })).toHaveCount(0);
    await expect(confirm).toBeDisabled();
    await expect(model).toBeDisabled();
    await expect(send).toHaveAttribute('data-shared-action', 'yes');
    expect(await Promise.all(controls.map((control) => control.boundingBox()))).toEqual(before);
    const commandColor = await page.getByTestId('composer-mode').evaluate(
        (node) => getComputedStyle(node).backgroundColor);
    expect(commandColor).not.toBe(messageColor);
    await page.getByLabel('command input').fill('Refresh');
    await send.click();
    await expect.poll(async () => (await requests(page, 'status_snapshot')).length).toBe(1);
    await page.keyboard.press('Alt+Enter');
    await expect(confirm).toBeEnabled();
    await expect(model).toBeEnabled();
    await expect(page.getByTestId('composer-mode')).toHaveText('message mode');
});
