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
            const dialog = page.getByRole('dialog');
            await expect(dialog).toBeVisible();
            await expect(dialog).toContainText('Argument preview only');
            await expect(dialog).toContainText('The decision applies to the original operation');
            await dialog.locator('details summary').click();
            const details = dialog.locator('details pre');
            await expect(details).toContainText('display_truncated');
            if (kind === 'fields') {
                for (let index = 0; index < 10; index++) await expect(details).toContainText(`field_${index}`);
            } else {
                await expect(dialog.locator('pre').first()).toContainText('printf');
                await expect(details).toContainText('/workspace');
            }
            expect(await details.evaluate(node => node.scrollHeight > node.clientHeight)).toBe(true);
            const geometry = await dialog.evaluate(node => {
                const box = node.getBoundingClientRect();
                return { top: box.top, bottom: box.bottom, height: innerHeight,
                    overflow: document.documentElement.scrollWidth - document.documentElement.clientWidth };
            });
            expect(geometry.top).toBeGreaterThanOrEqual(0);
            expect(geometry.bottom).toBeLessThanOrEqual(geometry.height);
            expect(geometry.overflow).toBeLessThanOrEqual(1);
            for (const name of ['Approve', 'Deny', 'Later']) {
                await expect(dialog.getByRole('button', { name, exact: true })).toBeInViewport();
            }
            await page.screenshot({ path: testInfo.outputPath('approval-preview.png'), animations: 'disabled' });
            await dialog.getByRole('button', { name: 'Approve', exact: true }).click();
            const received = await page.request.get(`${STUB}/__stub/decisions`);
            expect((await received.json()).decisions).toHaveLength(1);
        });
    }
}
