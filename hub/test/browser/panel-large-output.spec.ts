/** Opt-in measurement, not a universal timing gate. See README benchmark notes. */
import { readFileSync, writeFileSync } from 'node:fs';
import { expect, test } from '@playwright/test';
import { emit, open } from './harness.ts';

test('measures completed large output rendering and composer responsiveness', async ({ page }) => {
    test.skip(process.env.SIMPLEX_LARGE_OUTPUT_BENCHMARK !== '1');
    test.setTimeout(180000);
    const input = JSON.parse(readFileSync(process.env.SIMPLEX_OUTPUT_CASES!, 'utf8')) as
        { bytes: number; data: unknown; envelopeBytes: number }[];
    const results: unknown[] = [];
    for (const item of input) {
        await open(page, '?session=demo&panel_profile=1');
        await page.evaluate(() => {
            const target = window as unknown as { __tasks: number[] };
            target.__tasks = [];
            new PerformanceObserver(entries => {
                for (const entry of entries.getEntries()) target.__tasks.push(entry.duration);
            }).observe({ entryTypes: ['longtask'] });
        });
        await emit(page, 'run_started', {});
        const started = Date.now();
        await emit(page, 'model_response', item.data);
        await expect(page.getByTestId('assistant-message')).toContainText('Final benchmark answer');
        const rendered = Date.now() - started;
        const composer = page.locator('textarea').first();
        const inputStarted = Date.now();
        await composer.fill('Responsive input');
        await expect(composer).toHaveValue('Responsive input');
        const expanded = Date.now();
        await page.getByTestId('assistant-message').locator('summary').filter({ hasText: 'reasoning' }).click();
        await page.waitForTimeout(50);
        const measurements = await page.evaluate(() => {
            const target = window as unknown as { __tasks: number[];
                __simplexPanelProfile: { read(): unknown }; performance: Performance & { memory?: { usedJSHeapSize: number } } };
            return { longTasks: target.__tasks, heapBytes: target.performance.memory?.usedJSHeapSize ?? null,
                counters: target.__simplexPanelProfile.read(), domNodes: document.querySelectorAll('*').length,
                userAgent: navigator.userAgent };
        });
        results.push({ bytes: item.bytes, envelopeBytes: item.envelopeBytes, renderMs: rendered,
            inputMs: expanded - inputStarted, expandMs: Date.now() - expanded, ...measurements });
        writeFileSync(process.env.SIMPLEX_OUTPUT_REPORT!, JSON.stringify(results, null, 2));
    }
});
