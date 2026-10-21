import { test, expect, type Page } from '@playwright/test';
import { open, STUB, setSessions } from './harness.ts';

const document = { id: 'review', name: 'review', title: 'Review changes',
    description: 'Inspect correctness.', keywords: ['code'], revision: 'one', text: 'Review carefully.' };

async function library(page: Page) {
    let current = { ...document };
    await page.route('**/api/skills', route => route.fulfill({ json: {
        skills: [current], errors: [],
    } }));
    await page.route('**/api/skills/*', route => route.request().url().endsWith('/review')
        ? route.fulfill({ json: current })
        : route.fulfill({ status: 404, json: { message: 'Skill not found' } }));
    return { change: () => { current = { ...document, revision: 'two', text: 'Updated instructions.' }; } };
}
async function inputs(page: Page) {
    return ((await (await page.request.get(`${STUB}/__stub/received`)).json()).received as
        Array<{ type: string; content?: Array<{ raw: string }>; operation?: string; request_id?: string; session?: string }>)
        .filter(item => item.type === 'input');
}

test('picker inserts at the caret and sends an in-place expanded snapshot', async ({ page }) => {
    await library(page);
    await open(page, '?session=demo');
    const input = page.getByLabel('message', { exact: true });
    await input.fill('Before  after');
    await input.evaluate((node: HTMLTextAreaElement) => node.setSelectionRange(7, 7));
    await page.getByRole('button', { name: 'Choose a skill' }).click();
    await page.getByRole('button', { name: /Review changes/ }).click();
    await expect(input).toHaveValue('Before @{skill:review} after');
    await page.getByText('Preview message', { exact: true }).click();
    await expect(page.locator('form pre')).toHaveText('Before Review carefully. after');
    await page.getByRole('button', { name: 'Send', exact: true }).click();
    await expect.poll(async () => (await inputs(page)).at(-1)?.content?.[0]?.raw)
        .toBe('Before Review carefully. after');
    await expect(input).toHaveValue('');
});

test('typed references complete, delete and escape', async ({ page }) => {
    await library(page);
    await open(page, '?session=demo');
    const input = page.getByLabel('message', { exact: true });
    await input.fill('Task @{skill:rev');
    await expect(page.getByRole('option', { name: /Review changes/ })).toBeVisible();
    await input.press('ArrowDown');
    await input.press('Enter');
    await expect(input).toHaveValue('Task @{skill:review}');
    await expect(input).toHaveValue('Task @{skill:review}');
    await input.fill('Task @{skill:missing}');
    await expect(page.getByRole('button', { name: 'Send', exact: true })).toBeDisabled();
    await input.fill('@{skill:constructor}');
    await expect(page.getByRole('button', { name: 'Send', exact: true })).toBeDisabled();
    await input.fill('Literal \\@{skill:missing}');
    await page.getByRole('button', { name: 'Send', exact: true }).click();
    await expect.poll(async () => (await inputs(page)).at(-1)?.content?.[0]?.raw).toBe('Literal @{skill:missing}');
    await input.fill('Plain request');
    await expect(page.getByText('Preview message', { exact: true })).toHaveCount(0);
});

test('file changes require an explicit snapshot refresh', async ({ page }) => {
    const files = await library(page);
    await open(page, '?session=demo');
    await page.getByLabel('message', { exact: true }).fill('@{skill:review} task');
    await expect(page.getByRole('button', { name: 'Send', exact: true })).toBeEnabled();
    files.change();
    await page.getByRole('button', { name: 'Choose a skill' }).click();
    await expect(page.getByText('Skill file changed.', { exact: false })).toBeVisible();
    await page.keyboard.press('Escape');
    await page.getByText('Preview message', { exact: true }).click();
    await expect(page.locator('form pre')).toHaveText('Review carefully. task');
    await page.getByRole('button', { name: 'Refresh skill text', exact: true }).click();
    await expect(page.locator('form pre')).toHaveText('Updated instructions. task');
});

for (const suffix of [' @{skill:', ' @{skill:invalid id}']) {
    test(`editing another reference preserves loaded snapshots: ${suffix}`, async ({ page }) => {
        const files = await library(page);
        await open(page, '?session=demo');
        const input = page.getByLabel('message', { exact: true });
        const send = page.getByRole('button', { name: 'Send', exact: true });
        await input.fill('@{skill:review} task');
        await expect(send).toBeEnabled();
        files.change();
        await input.fill(`@{skill:review} task${suffix}`);
        await expect(send).toBeDisabled();
        await input.fill('@{skill:review} task');
        await expect(send).toBeEnabled();
        await page.getByText('Preview message', { exact: true }).click();
        await expect(page.locator('form pre')).toHaveText('Review carefully. task');
        await send.click();
        await expect.poll(async () => (await inputs(page)).at(-1)?.content?.[0]?.raw)
            .toBe('Review carefully. task');
    });
}

for (const refusal of ['refuseInput', 'rejectInput']) {
    test(`${refusal} restores markers without expanding twice`, async ({ page }) => {
        await library(page);
        await open(page, '?session=demo');
        await page.request.post(`${STUB}/__stub/settings`, { data: { [refusal]: true } });
        const input = page.getByLabel('message', { exact: true });
        await input.fill('@{skill:review} task');
        await page.getByRole('button', { name: 'Send', exact: true }).click();
        await expect.poll(async () => (await inputs(page)).length).toBe(1);
        await expect(input).toHaveValue('@{skill:review} task');
        await page.request.post(`${STUB}/__stub/settings`, { data: { [refusal]: false } });
        await page.getByRole('button', { name: 'Send', exact: true }).click();
        await expect.poll(async () => (await inputs(page)).length).toBe(2);
        expect((await inputs(page)).map(item => item.content?.[0]?.raw)).toEqual([
            'Review carefully. task', 'Review carefully. task',
        ]);
    });
}

test('late rejection preserves a newer draft and offers explicit recovery', async ({ page }) => {
    await library(page);
    await open(page, '?session=demo');
    await page.request.post(`${STUB}/__stub/settings`, { data: { holdInput: true } });
    const input = page.getByLabel('message', { exact: true });
    await input.fill('@{skill:review} old task');
    await page.getByRole('button', { name: 'Send', exact: true }).click();
    await expect.poll(async () => (await inputs(page)).length).toBe(1);
    const sent = (await inputs(page))[0]!;
    await input.fill('New draft');
    await page.request.post(`${STUB}/__stub/message`, { data: { type: 'error', error: 'input_not_sent',
        session: 'demo', message: 'refused', request: sent } });
    await expect(page.getByRole('button', { name: 'Restore rejected draft' })).toBeVisible();
    await expect(input).toHaveValue('New draft');
    await page.getByRole('button', { name: 'Restore rejected draft' }).click();
    await expect(input).toHaveValue('@{skill:review} old task');
});

test('switching sessions discards markers and late skill reads', async ({ page }) => {
    await library(page);
    await open(page);
    await setSessions(page, ['demo', 'other']);
    await page.reload();
    await page.getByTestId('session-row').filter({ hasText: 'demo' }).click();
    let release!: () => void;
    let started!: () => void;
    const gate = new Promise<void>(resolve => { release = resolve; });
    const requested = new Promise<void>(resolve => { started = resolve; });
    await page.route('**/api/skills/review', async route => {
        started();
        await gate;
        await route.fulfill({ json: document });
    }, { times: 1 });
    await page.getByLabel('message', { exact: true }).fill('@{skill:review} task');
    await requested;
    await page.getByTestId('session-row').filter({ hasText: 'other' }).click();
    release();
    await expect(page.getByLabel('message', { exact: true })).toHaveValue('');
    await expect(page.getByText('Preview message', { exact: true })).toHaveCount(0);
});
