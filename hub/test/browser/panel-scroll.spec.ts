import { expect, test, type Locator, type Page } from '@playwright/test';
import { call, emit, modelResponse, open, setSessions, STUB, toolResult } from './harness.ts';

async function frames(page: Page) {
    await page.evaluate(() => new Promise<void>(resolve => requestAnimationFrame(() =>
        requestAnimationFrame(() => resolve()))));
}

/** One whole turn, with identities an assertion can tell apart. */
function turnEvents(index: number) {
    const extra = { request_id: `request-${index}`, run_id: `run-${index}` };
    return [
        { event: 'input_admitted', data: { operation: 'message' }, extra },
        { event: 'run_started', data: {}, extra },
        { event: 'model_response', data: modelResponse(`Round ${index}.\n\n`.repeat(8)), extra },
        { event: 'tool_calls', data: [call(`tool-${index}`, 'run_command', { command: `echo ${index}` })], extra },
        { event: 'tool_results', data: [toolResult(`tool-${index}`, 'run_command', `out ${index}`)], extra },
        { event: 'run_finished', data: { status: 'completed', exchanges: 1 }, extra },
    ];
}

/**
 * Sample the transcript's distance from its last line on every frame, from
 * before the bundle runs. A sample is taken before the frame's paint, so a gap
 * recorded here is a frame that painted the conversation above its end.
 */
async function sampleBeforeLoad(page: Page) {
    await page.addInitScript(() => {
        const target = window as unknown as { __scrollSamples?: { top: number; gap: number }[] };
        target.__scrollSamples = [];
        const sample = () => {
            const node = document.querySelector('[data-testid="transcript"]');
            if (node) target.__scrollSamples!.push({
                top: node.scrollTop,
                gap: node.scrollHeight - node.scrollTop - node.clientHeight,
            });
            requestAnimationFrame(sample);
        };
        requestAnimationFrame(sample);
    });
}

async function startSampling(page: Page) {
    await page.evaluate(() => {
        const target = window as unknown as { __scrollSamples?: { top: number; gap: number }[] };
        target.__scrollSamples = [];
        const sample = () => {
            const node = document.querySelector('[data-testid="transcript"]');
            if (node) target.__scrollSamples!.push({
                top: node.scrollTop,
                gap: node.scrollHeight - node.scrollTop - node.clientHeight,
            });
            requestAnimationFrame(sample);
        };
        requestAnimationFrame(sample);
    });
}

async function readSamples(page: Page) {
    return page.evaluate(() => (window as unknown as
        { __scrollSamples: { top: number; gap: number }[] }).__scrollSamples);
}

/** The `history` requests the panel has sent, oldest first. */
async function historyQueries(page: Page): Promise<{ request_id: string }[]> {
    const response = await page.request.get(`${STUB}/__stub/received`);
    const received = (await response.json()).received as { type: string; request_id?: string }[];
    return received.filter((message): message is { type: string; request_id: string } =>
        message.type === 'history' && typeof message.request_id === 'string');
}

/** Restored turns long enough that three of them are taller than the viewport. */
function historyTurns(count: number) {
    return Array.from({ length: count }, (_, index) => {
        const execution = { worker_id: 'stub-worker', request_id: `restored-${index}`,
            run_id: `restored-run-${index}` };
        return {
            index,
            source: execution,
            user: [{ type: 'text', modality: 'text', raw: `Restored input ${index}` }],
            steps: [{
                index: 0,
                execution,
                commit_sequence: '1',
                content: [{ type: 'text', modality: 'text',
                    raw: `Restored answer ${index}.\n\n`.repeat(12) }],
                tool_calls: 0,
            }],
            omitted_steps: 0,
        };
    });
}

/** Count actual DOM work over twenty frame-separated input positions; never assert timings. */
async function sampleScrollWork(scroller: Locator) {
    return scroller.evaluate(async node => {
        const work = { roundQueries: 0, childQueries: 0, lineQueries: 0,
            roundBounds: 0, childBounds: 0 };
        const originalQuery = Element.prototype.querySelectorAll;
        const originalBounds = Element.prototype.getBoundingClientRect;
        Element.prototype.querySelectorAll = function (this: Element, selector: string) {
            if (selector.includes('[data-testid="round"]')) work.roundQueries += 1;
            if (selector.includes('[data-testid="assistant-message"]')) work.childQueries += 1;
            if (selector.includes('[data-testid="conversation-line"]')) work.lineQueries += 1;
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

/** Count full-line queries while one live response is appended and reconciled. */
async function sampleArrivalWork(page: Page, scroller: Locator): Promise<number> {
    await page.evaluate(() => {
        const target = window as unknown as { __lineQueries?: number; __restoreLineQueries?: () => void };
        target.__lineQueries = 0;
        const original = Element.prototype.querySelectorAll;
        target.__restoreLineQueries = () => { Element.prototype.querySelectorAll = original; };
        Element.prototype.querySelectorAll = function (this: Element, selector: string) {
            if (selector.includes('[data-testid="conversation-line"]')) {
                target.__lineQueries = (target.__lineQueries ?? 0) + 1;
            }
            return original.call(this, selector);
        } as typeof original;
    });
    await emit(page, 'model_response', modelResponse('Arrived while sampling.\n\n'.repeat(20)));
    await expect(scroller).toContainText('Arrived while sampling.');
    await frames(page);
    return page.evaluate(() => {
        const target = window as unknown as { __lineQueries: number; __restoreLineQueries: () => void };
        const count = target.__lineQueries;
        target.__restoreLineQueries();
        return count;
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
    expect(report.lineQueries).toBe(0);
    expect(report.roundBounds).toBeGreaterThan(0);
    expect(report.roundBounds).toBeLessThan(20 * 16);
    // Keep the probe installed through a live append: reconciliation itself
    // must not enumerate the retained lines.
    expect(await sampleArrivalWork(page, scroller)).toBe(0);
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

test('content growth in the same frame does not swallow upward reader movement', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('Growing transcript.\n\n'.repeat(100)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await frames(page);
    const moved = await scroller.evaluate(node => {
        const before = node.scrollTop;
        node.dispatchEvent(new WheelEvent('wheel'));
        const extra = document.createElement('div');
        extra.style.height = '300px';
        node.firstElementChild!.append(extra);
        node.scrollTop = before - 200;
        return node.scrollTop;
    });
    await frames(page);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    expect(Math.abs(await scroller.evaluate(node => node.scrollTop) - moved)).toBeLessThanOrEqual(2);
    await emit(page, 'model_response', modelResponse('More output after the gesture.\n\n'.repeat(10)));
    await frames(page);
    expect(Math.abs(await scroller.evaluate(node => node.scrollTop) - moved)).toBeLessThanOrEqual(2);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    await page.getByRole('button', { name: 'jump to latest' }).click();
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
});

test('a conversation paints at its latest message, not at its top', async ({ page }) => {
    await page.request.post(`${STUB}/__stub/reset`);
    await page.request.post(`${STUB}/__stub/emit-batch`, { data: {
        session: 'demo', seedOnly: true,
        events: Array.from({ length: 20 }, (_, index) => turnEvents(index)).flat(),
    } });
    await sampleBeforeLoad(page);
    await page.goto('/?session=demo');
    await expect(page.getByTestId('round')).toHaveCount(20);
    await frames(page);
    const samples = await readSamples(page);
    expect(samples.length).toBeGreaterThan(3);
    // The write lands in the commit's layout effect, so no frame ever painted
    // the conversation above its last message. Deferring it to the next
    // animation frame used to paint one frame of the top of the session.
    expect(Math.max(...samples.map(sample => sample.gap))).toBeLessThan(2);
    expect(samples.at(-1)!.top).toBeGreaterThan(100);
});

test('switching sessions paints the new conversation at its latest message', async ({ page }) => {
    await page.request.post(`${STUB}/__stub/reset`);
    await setSessions(page, ['demo', 'other']);
    await page.request.post(`${STUB}/__stub/emit-batch`, { data: { session: 'demo', seedOnly: true,
        events: Array.from({ length: 20 }, (_, index) => turnEvents(index)).flat() } });
    await page.request.post(`${STUB}/__stub/emit-batch`, { data: { session: 'other', seedOnly: true,
        events: Array.from({ length: 16 }, (_, index) => turnEvents(200 + index)).flat() } });
    await sampleBeforeLoad(page);
    await page.goto('/?session=demo');
    await expect(page.getByTestId('round')).toHaveCount(20);
    await frames(page);
    const from = await page.evaluate(() => (window as unknown as
        { __scrollSamples: unknown[] }).__scrollSamples.length);
    await page.locator('[data-testid="session-row"][data-session="other"]').click();
    await expect(page.getByTestId('transcript')).toContainText('Round 215.');
    await frames(page);
    const samples = (await readSamples(page)).slice(from);
    expect(samples.length).toBeGreaterThan(2);
    expect(Math.max(...samples.map(sample => sample.gap))).toBeLessThan(2);
});

test('growth while following glides to the latest message', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('Opening.\n\n'.repeat(40)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    // The quiet window is what separates a message arriving from hydration
    // replaying; wait past it so this growth is treated as a message.
    await page.waitForTimeout(300);
    await startSampling(page);
    await emit(page, 'model_response', modelResponse('A reply that arrives after a pause.\n\n'.repeat(8)));
    await expect(page.getByTestId('transcript')).toContainText('A reply that arrives after a pause.');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    const gaps = (await readSamples(page)).map(sample => sample.gap);
    expect(gaps.length).toBeGreaterThan(3);
    // It moved through positions between the old end and the new one instead of
    // teleporting, and it never moved away from the end on the way there.
    const first = gaps.findIndex(gap => gap > 2);
    expect(first).toBeGreaterThan(-1);
    const glide = gaps.slice(first).filter(gap => gap < 400);
    expect(glide.length).toBeGreaterThan(2);
    for (let index = 1; index < glide.length; index += 1) {
        expect(glide[index]!).toBeLessThanOrEqual(glide[index - 1]! + 1);
    }
    await expect(page.getByRole('button', { name: 'jump to latest' })).toHaveCount(0);
});

test('jump to latest moves instead of teleporting, and a wheel takes it back', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('Long read.\n\n'.repeat(120)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await scroller.hover();
    await page.mouse.wheel(0, -1500);
    const jump = page.getByRole('button', { name: 'jump to latest' });
    await expect(jump).toBeVisible();
    await startSampling(page);
    await jump.click();
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    const positions = [...new Set((await readSamples(page)).map(sample => sample.top))];
    expect(positions.length).toBeGreaterThanOrEqual(3);

    // A gesture mid-glide owns the position again: nothing keeps moving under it.
    await scroller.hover();
    await page.mouse.wheel(0, -600);
    await frames(page);
    const held = await scroller.evaluate(node => node.scrollTop);
    await frames(page);
    expect(Math.abs(await scroller.evaluate(node => node.scrollTop) - held)).toBeLessThanOrEqual(2);
    await expect(jump).toBeVisible();
});

test('jump to latest lands at once when the reader asks for less motion', async ({ page }) => {
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('Long read.\n\n'.repeat(120)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await scroller.hover();
    await page.mouse.wheel(0, -1500);
    const jump = page.getByRole('button', { name: 'jump to latest' });
    await expect(jump).toBeVisible();
    await startSampling(page);
    await jump.click();
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    const positions = [...new Set((await readSamples(page)).map(sample => sample.top))];
    expect(positions.length).toBeLessThanOrEqual(2);
});

test('a restored page that arrives late lands instead of sweeping', async ({ page }) => {
    await open(page);
    const turns = historyTurns(8);
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true, historyTurns: turns, historyResponses: [{ __hold: true }],
    } });
    await emit(page, 'ready', { active: false, capabilities: ['session-history'] },
        { request_id: '', run_id: '' });
    await page.goto('/?session=demo');
    const scroller = page.getByTestId('transcript');
    await expect(scroller).toBeVisible();
    await expect.poll(async () => (await historyQueries(page)).length).toBe(1);
    // Past the quiet window: on the clock alone this page would look like a
    // message arriving, and the viewport would sweep from the top to the end.
    await page.waitForTimeout(400);
    await startSampling(page);
    const query = (await historyQueries(page))[0]!;
    await emit(page, 'history', { request_id: query.request_id, start: 0, step: 0,
        next: turns.length, next_step: 0, total: turns.length, revision: 1, turns });
    await expect(page.getByTestId('history-turn')).toHaveCount(turns.length);
    await frames(page);
    const samples = await readSamples(page);
    expect(samples.length).toBeGreaterThan(3);
    // A restore is not a scroll: the reader is already at the end of what they
    // had, so the page lands there.
    expect(Math.max(...samples.map(sample => sample.gap))).toBeLessThan(2);
});

test('a replaced transcript tail lands instead of gliding', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('Original tail.\n\n'.repeat(8)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    // Let the previous growth leave the replay quiet window.
    await page.waitForTimeout(300);
    await startSampling(page);
    await page.evaluate(() => {
        const content = document.querySelector('[data-testid="reading-surface"]')!;
        const lines = content.querySelectorAll('[data-testid="conversation-line"]');
        const tail = lines[lines.length - 1]!;
        const replacement = tail.cloneNode(true) as HTMLElement;
        replacement.style.height = '720px';
        tail.replaceWith(replacement);
    });
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await frames(page);
    const tops = (await readSamples(page)).map(sample => sample.top);
    // A replacement is a restore. It may land at the new end, but it must not
    // animate through intermediate positions like a live append.
    expect(new Set(tops).size).toBeLessThanOrEqual(2);
});

test('a single large arrival moves in bounded steps', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('Opening.\n\n'.repeat(20)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await page.waitForTimeout(300);
    await startSampling(page);
    await emit(page, 'model_response', modelResponse('A very long answer.\n\n'.repeat(150)));
    await expect(page.getByTestId('transcript')).toContainText('A very long answer.');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    const tops = (await readSamples(page)).map(sample => sample.top);
    expect(new Set(tops).size).toBeGreaterThan(3);
    const steps = tops.slice(1).map((top, index) => Math.abs(top - tops[index]!));
    // The glide is capped per frame, so a three-thousand-pixel answer is still
    // followed frame by frame instead of losing a third of it at once.
    expect(Math.max(...steps)).toBeLessThanOrEqual(84);
});
