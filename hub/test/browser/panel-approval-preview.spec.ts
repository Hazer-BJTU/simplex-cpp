import { expect, test } from '@playwright/test';
import { STUB, open } from './harness.ts';

for (const width of [390, 1440]) {
    for (const kind of ['command', 'fields']) {
        test(`bounded ${kind} approval preview remains readable and actionable at ${width}px`, async ({ page }, testInfo) => {
            await page.setViewportSize({ width, height: 780 });
            await open(page, '?session=demo');
            const args = kind === 'command'
                ? { command: 'printf "漢字😀\\n"; '.repeat(10000), cwd: '/workspace' }
                : Object.fromEntries(Array.from({ length: 10 }, (_, index) => [`field_${index}`, 'value\n'.repeat(3000)]));
            await page.request.post(`${STUB}/__stub/confirm`, { data: {
                confirmation_id: 'budget-preview', call: { name: 'run_command', arguments: args },
            } });
            // The decision is answered in the page, so the evidence for it has
            // to be bounded where it sits: a long call scrolls inside its own
            // preview instead of pushing the actions out of reach.
            const block = page.getByTestId('approval-block');
            await expect(block).toBeVisible();
            await expect(block).toContainText('Argument preview only');
            await expect(block).toContainText('The decision applies to the original operation');
            const details = block.getByTestId('approval-arguments');
            // The evidence is rendered only when it is asked for; a bounded
            // preview can still be tens of kilobytes.
            await expect(details).toHaveCount(0);
            await block.getByTestId('approval-disclosure').click();
            await expect(details).toContainText('display_truncated');
            if (kind === 'fields') {
                for (let index = 0; index < 10; index++) await expect(details).toContainText(`field_${index}`);
            } else {
                await expect(block.locator('pre').first()).toContainText('printf');
                await expect(details).toContainText('/workspace');
            }
            expect(await details.evaluate(node => node.scrollHeight > node.clientHeight)).toBe(true);
            const geometry = await block.evaluate(node => {
                const box = node.getBoundingClientRect();
                return { top: box.top, bottom: box.bottom, height: innerHeight,
                    overflow: document.documentElement.scrollWidth - document.documentElement.clientWidth };
            });
            expect(geometry.top).toBeGreaterThanOrEqual(0);
            expect(geometry.bottom).toBeLessThanOrEqual(geometry.height);
            expect(geometry.overflow).toBeLessThanOrEqual(1);
            for (const name of ['Approve', 'Deny']) {
                await expect(block.getByRole('button', { name, exact: true })).toBeInViewport();
            }
            await page.screenshot({ path: testInfo.outputPath('approval-preview.png'), animations: 'disabled' });
            await block.getByRole('button', { name: 'Approve', exact: true }).click();
            const received = await page.request.get(`${STUB}/__stub/decisions`);
            expect((await received.json()).decisions).toHaveLength(1);
        });
    }
}
