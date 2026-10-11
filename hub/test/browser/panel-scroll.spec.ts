import { expect, test, type Locator, type Page } from '@playwright/test';
import { call, emit, modelResponse, open, STUB, toolResult } from './harness.ts';

async function frames(page: Page) {
    await page.evaluate(() => new Promise<void>(resolve => requestAnimationFrame(() =>
        requestAnimationFrame(() => resolve()))));
}

/** Count actual DOM work over twenty frame-separated input positions; never assert timings. */
async function sampleScrollWork(scroller: Locator) {
    return scroller.evaluate(async node => {
        const work = { roundQueries: 0, childQueries: 0, roundBounds: 0, childBounds: 0 };
        const originalQuery = Element.prototype.querySelectorAll;
        const originalBounds = Element.prototype.getBoundingClientRect;
        Element.prototype.querySelectorAll = function (this: Element, selector: string) {
            if (selector.includes('[data-testid="round"]')) work.roundQueries += 1;
            if (selector.includes('[data-testid="assistant-message"]')) work.childQueries += 1;
            return originalQuery.call(this, selector);
        } as typeof originalQuery;
        Element.prototype.getBoundingClientRect = function () {
            if (this.matches('[data-testid="round"], [data-testid="history-turn"]')) work.roundBounds += 1;
            if (this.matches('[data-testid="assistant-message"], [data-testid="tool-card"], [data-testid="outbox-item"]')) work.childBounds += 1;
            return originalBounds.call(this);
        };
        try {
            for (let index = 0; index < 20; index += 1) {
                node.dispatchEvent(new WheelEvent('wheel'));
                node.scrollTop -= 6;
                await new Promise<void>(resolve => requestAnimationFrame(() => resolve()));
            }
            await new Promise<void>(resolve => requestAnimationFrame(() => requestAnimationFrame(() => resolve())));
            return { ...work, rounds: originalQuery.call(node, '[data-testid="round"]').length,
                scrollPositions: 20, top: node.scrollTop };
        } finally {
            Element.prototype.querySelectorAll = originalQuery;
            Element.prototype.getBoundingClientRect = originalBounds;
        }
    });
}

test('a long transcript reuses membership and probes logarithmically during reading', async ({ page }) => {
    await open(page, '?session=demo');
    const events = Array.from({ length: 150 }, (_, index) => {
        const extra = { request_id: `request-${index}`, run_id: `run-${index}` };
        return [
            { event: 'input_admitted', data: { operation: 'message' }, extra },
            { event: 'model_response', data: modelResponse(`Round ${index}.\n\n`.repeat(20)), extra },
            { event: 'run_finished', data: { status: 'completed' }, extra },
        ];
    }).flat();
    await page.request.post(`${STUB}/__stub/emit-batch`, { data: { events } });
    const scroller = page.getByTestId('transcript');
    await expect(page.getByTestId('round')).toHaveCount(150);
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await scroller.hover();
    const delta = await scroller.evaluate(node => node.querySelectorAll('[data-testid="round"]')[140]!
        .getBoundingClientRect().top - node.getBoundingClientRect().top + 10);
    await page.mouse.wheel(0, delta);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    await frames(page);
    const report = await sampleScrollWork(scroller);
    console.log('scroll-work', JSON.stringify(report));
    expect(report.rounds).toBe(150);
    expect(report.roundQueries).toBe(0);
    expect(report.childQueries).toBeLessThan(10);
    expect(report.roundBounds).toBeGreaterThan(0);
    expect(report.roundBounds).toBeLessThan(20 * 16);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    await page.getByRole('button', { name: 'jump to latest' }).click();
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
});

test('a large nested tool list reuses child membership during reading', async ({ page }) => {
    await open(page, '?session=demo');
    const events = Array.from({ length: 4 }, (_, exchange) => {
        const calls = Array.from({ length: 64 }, (_, index) =>
            call(`tool-${exchange}-${index}`, 'run_command', { command: `echo ${exchange}-${index}` }));
        return [
            { event: 'model_response', data: modelResponse(`Exchange ${exchange}`, { invokes: calls }) },
            { event: 'tool_calls', data: calls },
            { event: 'tool_results', data: calls.map(query => toolResult(query.id, query.name, 'done')) },
        ];
    }).flat();
    await page.request.post(`${STUB}/__stub/emit-batch`, { data: { events } });
    const scroller = page.getByTestId('transcript');
    await expect(page.getByTestId('tool-card')).toHaveCount(256);
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await scroller.hover();
    const delta = await page.getByTestId('tool-card').nth(210).evaluate(element =>
        element.getBoundingClientRect().top - element.closest('[data-testid="transcript"]')!.getBoundingClientRect().top + 10);
    await page.mouse.wheel(0, delta);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    await frames(page);
    const report = await sampleScrollWork(scroller);
    console.log('nested-scroll-work', JSON.stringify(report));
    expect(report.roundQueries).toBe(0);
    expect(report.childQueries).toBe(0);
    expect(report.childBounds).toBeGreaterThan(0);
    expect(report.childBounds).toBeLessThan(20 * 16);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
});

test('nested tool anchors survive earlier-content removal and refresh membership after disclosure changes', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('Earlier content.\n\n'.repeat(30), { invokes: [
        { id: 'anchor', name: 'run_command', arguments: { command: 'echo anchor' } },
    ] }));
    await emit(page, 'tool_results', [{ query: { id: 'anchor', name: 'run_command' },
        output: { type: 'text', raw: 'Anchored output\n'.repeat(40) } }]);
    const scroller = page.getByTestId('transcript');
    const tool = page.getByTestId('tool-card');
    await tool.getByTestId('tool-details').locator('summary').click();
    await emit(page, 'model_response', modelResponse('Later content.\n\n'.repeat(60)));
    await frames(page);
    await scroller.hover();
    const delta = await tool.evaluate(element => element.getBoundingClientRect().top
        - element.closest('[data-testid="transcript"]')!.getBoundingClientRect().top + 50);
    await page.mouse.wheel(0, delta);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    const offset = () => tool.evaluate(element => element.getBoundingClientRect().top
        - element.closest('[data-testid="transcript"]')!.getBoundingClientRect().top);
    await expect.poll(offset).toBeLessThan(-10);
    const before = await offset();
    // A DOM removal inside the assistant keeps its tool mounted, reproducing
    // retention/disclosure above the exact nested item being read.
    expect(await page.getByTestId('assistant-message').first().evaluate(element => {
        const earlier = element.querySelector('.md');
        earlier?.remove();
        return earlier !== null;
    })).toBe(true);
    await frames(page);
    await expect.poll(async () => Math.abs(await offset() - before)).toBeLessThanOrEqual(2);
    await tool.getByTestId('tool-details').locator('summary').click();
    await tool.getByTestId('tool-details').locator('summary').click();
    await frames(page);
    await emit(page, 'model_response', modelResponse('Fresh output'));
    await expect(scroller).toContainText('Fresh output');
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
});

test('new keyboard and scrollbar intent wins after jump and hidden-pane reactivation', async ({ page }) => {
    await open(page, '?session=demo');
    await page.request.post(`${STUB}/__stub/plan`, { data: { plan: { markdown: '# Plan', revision: 1 } } });
    await emit(page, 'model_response', modelResponse('Read me.\n\n'.repeat(120)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await scroller.evaluate(node => { node.tabIndex = 0; node.focus(); });
    await page.keyboard.press('PageUp');
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    await page.getByRole('button', { name: 'jump to latest' }).click();
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await scroller.evaluate(node => {
        node.dispatchEvent(new PointerEvent('pointerdown'));
        node.scrollTop -= 400; // Native scrollbar movement shares this admission path.
    });
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    const before = await scroller.evaluate(node => node.scrollTop);
    await page.getByRole('tab', { name: 'Plan', exact: true }).click();
    await emit(page, 'model_response', modelResponse('Hidden arrival'));
    await page.getByRole('tab', { name: 'Conversation', exact: true }).click();
    await frames(page);
    expect(Math.abs(await scroller.evaluate(node => node.scrollTop) - before)).toBeLessThanOrEqual(2);
    await scroller.hover();
    await page.mouse.wheel(0, -200);
    await expect.poll(() => scroller.evaluate(node => node.scrollTop)).toBeLessThan(before - 100);
});

test('viewport clamping inside the input window does not restore following', async ({ page }) => {
    await page.setViewportSize({ width: 1280, height: 650 });
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('Reading near the end.\n\n'.repeat(100)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await scroller.hover();
    await page.mouse.wheel(0, -120);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    await page.setViewportSize({ width: 1280, height: 900 });
    await frames(page);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    const before = await scroller.evaluate(node => node.scrollTop);
    await emit(page, 'model_response', modelResponse('Arrived after viewport growth.\n\n'.repeat(30)));
    await frames(page);
    expect(Math.abs(await scroller.evaluate(node => node.scrollTop) - before)).toBeLessThanOrEqual(2);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
});
