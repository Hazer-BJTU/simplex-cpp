/** Top-level notices share the frame without stacking extra panel dividers. */
import { expect, test, type Locator, type Page } from '@playwright/test';
import { STUB, emit, modelResponse, open, stoppedSession, withSession } from './harness.ts';

test.use({ launchOptions: { ignoreDefaultArgs: ['--hide-scrollbars'] } });

/** Wait for the finite entrance before comparing independently measured boxes. */
async function finishEntrance(node: Locator): Promise<void> {
    await node.evaluate(async element => {
        await Promise.all(element.getAnimations().map(animation => animation.finished));
    });
}

async function showNotice(page: Page, message: string): Promise<void> {
    await page.request.post(`${STUB}/__stub/message`, { data: {
        type: 'error', error: 'worker_action_failed', message,
    } });
    await expect(page.locator('header [role="alert"]')).toContainText(message);
}

/** Ask about deletion without carrying out any destructive operation. */
async function askDelete(page: Page): Promise<Locator> {
    await page.getByRole('button', { name: 'More actions', exact: true }).click();
    await page.getByRole('menuitem', { name: /Delete session/ }).click();
    const prompt = page.getByTestId('session-confirm');
    await expect(prompt).toContainText('This cannot be undone.');
    await expect(prompt.getByRole('button', { name: 'Cancel', exact: true })).toBeFocused();
    await finishEntrance(prompt);
    return prompt;
}

for (const theme of ['light', 'dark'] as const) {
    for (const width of [360, 768, 1440]) {
        test(`unboxed lifecycle prompts and notices fit ${theme} ${width}px`, async ({ page }, testInfo) => {
            await page.setViewportSize({ width, height: 900 });
            await page.addInitScript(value => localStorage.setItem('simplex.panel.theme', value), theme);
            await open(page, '?session=demo');
            await withSession(page, stoppedSession('demo'));
            await page.reload();
            await expect(page.getByText('connected', { exact: true })).toBeVisible();
            await expect(page.getByTestId('session-title')).toHaveText('demo');
            await emit(page, 'model_response', modelResponse('Earlier conversation.\n\n'.repeat(80)));
            await page.addStyleTag({ content: `
                .reading-scroll { overflow-y: scroll !important; }
                .reading-scroll::-webkit-scrollbar { width: 16px; }
            ` });
            await page.request.post(`${STUB}/__stub/plan`, { data: {
                plan: { markdown: '- Review the session before deleting it', revision: 1, updated_at: null },
            } });
            await expect(page.getByRole('tablist', { name: 'conversation panels' })).toBeVisible();
            if (width === 1440) await page.getByRole('button', { name: 'Show inspector', exact: true }).click();

            await showNotice(page, 'The requested worker action was refused. Review the worker state before retrying.');
            const notice = page.locator('header [role="alert"]');
            const prompt = await askDelete(page);
            await finishEntrance(notice);
            await expect(page.locator('header')).toHaveCSS('border-bottom-width', '1px');
            for (const node of [page.getByTestId('session-header'), prompt, prompt.locator('.reading-width'),
                notice, page.getByRole('tablist', { name: 'conversation panels' })]) {
                for (const side of ['top', 'bottom', 'left', 'right']) {
                    await expect(node).toHaveCSS(`border-${side}-width`, '0px');
                }
            }
            for (const node of [prompt, notice]) {
                await expect(node).toHaveCSS('background-color', 'rgba(0, 0, 0, 0)');
                await expect(node).toHaveCSS('animation-name', 'line-enter');
                await expect(node).toHaveCSS('animation-duration', '0.14s');
            }
            // The drawer retains its own section divider; only the conversation
            // toolbar joins the borderless session area.
            if (width === 1440) await expect(page.getByRole('tablist', { name: 'context panes' }))
                .toHaveCSS('border-bottom-width', '1px');

            await expect.poll(async () => page.evaluate(() => {
                const box = (selector: string) => document.querySelector(selector)!.getBoundingClientRect();
                const prompt = box('[data-testid="session-confirm"] .reading-width');
                const reading = box('[data-testid="reading-surface"]');
                const composer = box('[data-testid="composer-surface"]');
                return Math.abs(prompt.left - reading.left) + Math.abs(prompt.width - reading.width)
                    + Math.abs(prompt.left - composer.left) + Math.abs(prompt.width - composer.width);
            })).toBeLessThan(1);
            for (const button of await prompt.getByRole('button').all()) {
                const action = (await button.boundingBox())!;
                const column = (await prompt.locator('.reading-width').boundingBox())!;
                expect(action.x).toBeGreaterThanOrEqual(column.x);
                expect(action.x + action.width).toBeLessThanOrEqual(column.x + column.width + 1);
            }
            expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
            if (width === 1440) {
                const path = testInfo.outputPath('header-notices.png');
                await page.screenshot({ path, animations: 'disabled' });
                await testInfo.attach('header-notices', { path, contentType: 'image/png' });
            }

            await notice.getByRole('button', { name: 'dismiss this message' }).click();
            await expect(notice).toHaveCount(0);
            await expect(prompt).toBeVisible();
            await prompt.getByRole('button', { name: 'Cancel', exact: true }).click();
            await expect(prompt).toHaveCount(0);
            await expect(page.getByRole('button', { name: 'More actions', exact: true })).toBeFocused();
            expect((await (await page.request.get(`${STUB}/__stub/actions`)).json()).actions).toEqual([]);
        });
    }
}

test('long notice text wraps without displacing its dismiss action or the session controls', async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 720 });
    await open(page, '?session=demo');
    await showNotice(page, 'unbroken-diagnostic/'.repeat(80));
    const notice = page.locator('header [role="alert"]');
    await finishEntrance(notice);
    await expect(notice.getByRole('button', { name: 'dismiss this message' })).toBeInViewport();
    expect(await notice.evaluate(element => element.scrollWidth <= element.clientWidth)).toBe(true);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    await expect(page.getByRole('button', { name: 'Start', exact: true })).toBeInViewport();
    await notice.getByRole('button', { name: 'dismiss this message' }).click();
    await expect(page.getByRole('button', { name: 'Start', exact: true })).toBeVisible();
});

test('warning and protocol-refusal notices keep severity and dismiss independently without extra frames', async ({ page }) => {
    await open(page, '?session=demo');
    await page.request.post(`${STUB}/__stub/message`, { data: {
        type: 'accepted', action: 'worker', session: 'demo',
        result: { ok: false, error: 'The worker is already running.' },
    } });
    const warning = page.locator('header [role="alert"]').filter({ hasText: 'The worker is already running.' });
    await expect(warning).toBeVisible();
    await page.request.post(`${STUB}/__stub/message`, { data: {
        v: -1, type: 'error', error: 'unexpected_version', message: 'A mismatched protocol frame',
    } });
    const refusal = page.locator('header [role="alert"]').filter({ hasText: 'unsupported_version' });
    await expect(refusal).toBeVisible();
    expect(await warning.evaluate(element => getComputedStyle(element).color))
        .not.toBe(await refusal.evaluate(element => getComputedStyle(element).color));
    for (const node of [warning, refusal]) {
        await expect(node).toHaveCSS('background-color', 'rgba(0, 0, 0, 0)');
        await expect(node).toHaveCSS('border-top-width', '0px');
        await expect(node.locator('svg')).toBeVisible();
    }
    await warning.getByRole('button', { name: 'dismiss this message' }).click();
    await expect(warning).toHaveCount(0);
    await expect(refusal).toBeVisible();
    await refusal.getByRole('button', { name: 'dismiss this message' }).click();
    await expect(page.locator('header [role="alert"]')).toHaveCount(0);
    await expect(page.getByText('connected', { exact: true })).toBeVisible();
});

test('the session approval reminder is plain status text and never takes focus', async ({ page }) => {
    await open(page, '?session=demo');
    const input = page.getByRole('textbox', { name: 'message', exact: true });
    await input.fill('Keep typing while a tool needs approval');
    await page.request.post(`${STUB}/__stub/confirm`, { data: { confirmation_id: 'quiet-reminder' } });
    const reminder = page.getByTestId('session-approval-count');
    await expect(reminder).toHaveText('1 approval');
    await expect(input).toBeFocused();
    for (const node of [reminder, reminder.locator(':scope > span')]) {
        await expect(node).toHaveCSS('box-shadow', 'none');
        await expect(node).toHaveCSS('background-color', 'rgba(0, 0, 0, 0)');
    }
    await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'quiet-reminder' } });
    await expect(reminder).toHaveCount(0);
    await expect(input).toHaveValue('Keep typing while a tool needs approval');
});

test('switching sessions clears a pending lifecycle decision without submitting it', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/sessions`, { data: {
        sessions: [stoppedSession('first'), stoppedSession('second')],
    } });
    await page.reload();
    await page.locator('[data-testid="session-row"][data-session="first"]').click();
    await askDelete(page);
    await page.locator('[data-testid="session-row"][data-session="second"]').click();
    await expect(page.getByTestId('session-confirm')).toHaveCount(0);
    expect((await (await page.request.get(`${STUB}/__stub/actions`)).json()).actions).toEqual([]);
});

test('authentication stays in the title frame and fits a narrow viewport', async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 720 });
    await page.route('**/api/meta', route => route.fulfill({ status: 401, json: { error: 'unauthorized' } }));
    await open(page);
    const form = page.getByRole('form', { name: 'Panel authentication' });
    await expect(form).toBeVisible();
    await expect(form).toHaveCSS('border-top-width', '0px');
    await expect(form).toHaveCSS('background-color', 'rgba(0, 0, 0, 0)');
    await expect(form.getByRole('button', { name: 'use token', exact: true })).toBeInViewport();
    expect(await form.evaluate(element => element.scrollWidth <= element.clientWidth)).toBe(true);
    await form.getByLabel('panel token', { exact: true }).fill('test-token');
    await expect(form.getByLabel('panel token', { exact: true })).toHaveValue('test-token');
});

test('lifecycle prompts and error notices respect reduced motion', async ({ page }) => {
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await open(page, '?session=demo');
    await withSession(page, stoppedSession('demo'));
    await page.reload();
    await expect(page.getByText('connected', { exact: true })).toBeVisible();
    await showNotice(page, 'Review the worker state.');
    const prompt = await askDelete(page);
    for (const node of [prompt, page.locator('header [role="alert"]')]) {
        await finishEntrance(node);
        expect(await node.evaluate(element => Number.parseFloat(getComputedStyle(element).animationDuration)))
            .toBeLessThan(0.05);
        await expect(node).toHaveCSS('opacity', '1');
    }
    await page.keyboard.press('Escape');
    await expect(prompt).toHaveCount(0);
    await expect(page.getByRole('button', { name: 'More actions', exact: true })).toBeFocused();
});
