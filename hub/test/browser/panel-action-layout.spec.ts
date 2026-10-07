import { expect, test, type Locator } from '@playwright/test';
import { STUB, emit, modelResponse, open } from './harness.ts';

// Exercise actual scrollbar width rather than Chromium's hidden headless bars.
test.use({ launchOptions: { ignoreDefaultArgs: ['--hide-scrollbars'] } });

/** Check the actual label and icon bounds, not only the button's outer box. */
async function expectContainedLabel(button: Locator): Promise<void> {
    const fits = await button.evaluate(element => {
        const box = element.getBoundingClientRect();
        const label = element.querySelector('span')!;
        const range = document.createRange();
        range.selectNodeContents(label);
        const lines = [...range.getClientRects()];
        const icon = element.querySelector('svg')!.getBoundingClientRect();
        return lines.length === 1 && [...lines, icon].every(rect =>
            rect.left >= box.left && rect.right <= box.right
            && rect.top >= box.top && rect.bottom <= box.bottom);
    });
    expect(fits).toBe(true);
}

for (const width of [360, 768, 1440]) {
    for (const mode of ['message', 'command']) {
        test(`cancel labels fit in ${mode} mode at ${width}px`, async ({ page }) => {
            await page.setViewportSize({ width, height: 900 });
            await open(page, '?session=demo');
            if (mode === 'command') await page.getByLabel('message', { exact: true }).press('Alt+Enter');
            const send = await page.getByRole('button', { name: 'Send', exact: true }).boundingBox();
            await emit(page, 'run_started', {});
            const cancel = page.getByRole('button', { name: 'Cancel run', exact: true });
            await expectContainedLabel(cancel);
            expect(await cancel.boundingBox()).toEqual(send);
            await cancel.click();
            const pending = page.getByRole('button', { name: 'Cancelling…', exact: true });
            await expect(pending).toBeDisabled();
            await expectContainedLabel(pending);
            expect(await pending.boundingBox()).toEqual(send);
        });
    }

    test(`deferred approval rows keep long content and actions inside ${width}px`, async ({ page }, testInfo) => {
        await page.setViewportSize({ width, height: 900 });
        await open(page, '?session=demo');
        for (let index = 0; index < 3; index++) {
            await page.request.post(`${STUB}/__stub/confirm`, { data: {
                session: 'demo', confirmation_id: `layout-${index}`,
                call: { name: 'run_command', arguments: { command: 'long-command/'.repeat(100) } },
            } });
            await expect(page.getByRole('dialog')).toBeVisible();
            await page.keyboard.press('Escape');
            await expect(page.getByRole('dialog')).toHaveCount(0);
        }
        const list = page.getByTestId('approvals');
        await expect(page.getByTestId('approval-banner')).toHaveCount(3);
        expect(await list.evaluate(element => element.scrollWidth <= element.clientWidth)).toBe(true);
        for (const row of await page.getByTestId('approval-banner').all()) {
            const bounds = (await row.boundingBox())!;
            for (const name of ['Review', 'Approve', 'Deny']) {
                const button = (await row.getByRole('button', { name, exact: true }).boundingBox())!;
                expect(button.x).toBeGreaterThanOrEqual(bounds.x);
                expect(button.x + button.width).toBeLessThanOrEqual(bounds.x + bounds.width + 1);
            }
        }
        await page.screenshot({ path: testInfo.outputPath('approval-rows.png'), animations: 'disabled' });
        await page.getByTestId('approval-banner').last().getByRole('button', { name: 'Review' }).click();
        await expect(page.getByRole('dialog')).toContainText('long-command/');
    });
}

test.describe('classic scrollbar alignment', () => {

    for (const width of [768, 1440]) {
        test(`approval actions and reading edges remain distinct and aligned at ${width}px`, async ({ page }) => {
            await page.setViewportSize({ width, height: 960 });
            await open(page, '?session=demo');
            await page.addStyleTag({ content: `
                .reading-scroll { overflow-y: scroll !important; }
                .reading-scroll::-webkit-scrollbar, .approval-list::-webkit-scrollbar { width: 16px; }
            ` });
            await emit(page, 'model_response', modelResponse('Long transcript.\n\n'.repeat(100)));
            const list = page.getByTestId('approvals');
            async function expectAligned(): Promise<void> {
                await expect.poll(async () => {
                    const reading = (await page.getByTestId('reading-surface').boundingBox())!;
                    const composer = (await page.getByTestId('composer-surface').boundingBox())!;
                    const approval = (await list.locator('.reading-width').boundingBox())!;
                    return Math.abs(reading.x - approval.x) + Math.abs(reading.width - approval.width)
                        + Math.abs(composer.x - approval.x) + Math.abs(composer.width - approval.width);
                }).toBeLessThan(1);
            }
            // Start without an approval scrollbar, then overflow the list itself.
            for (let index = 0; index < 7; index++) {
                await page.request.post(`${STUB}/__stub/confirm`, { data: {
                    session: 'demo', confirmation_id: `alignment-${index}`,
                    call: { name: 'run_command', arguments: { command: 'echo review' } },
                } });
                await expect(page.getByRole('dialog')).toBeVisible();
                await page.keyboard.press('Escape');
                await expect(page.getByRole('dialog')).toHaveCount(0);
                if (index === 0) {
                    expect(await list.evaluate((node: HTMLElement) => node.offsetWidth - node.clientWidth)).toBe(0);
                    await expectAligned();
                    const row = page.getByTestId('approval-banner').first();
                    const review = row.getByRole('button', { name: 'Review', exact: true });
                    const approve = row.getByRole('button', { name: 'Approve', exact: true });
                    const deny = row.getByRole('button', { name: 'Deny', exact: true });
                    const background = (button: Locator) => button.evaluate(node => getComputedStyle(node).backgroundColor);
                    const color = (button: Locator) => button.evaluate(node => getComputedStyle(node).color);
                    expect(await background(approve)).not.toBe(await background(review));
                    expect(await color(deny)).not.toBe(await color(review));
                }
            }
            await expect.poll(() => list.evaluate((node: HTMLElement) => node.offsetWidth - node.clientWidth)).toBe(16);
            await expectAligned();
            // Re-measure after the independent scrollbar disappears again.
            await page.addStyleTag({ content: '.approval-list { max-height: none; }' });
            await expect.poll(() => list.evaluate((node: HTMLElement) => node.offsetWidth - node.clientWidth)).toBe(0);
            await expectAligned();
        });
    }

});
