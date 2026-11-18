/** Reading-width overlays and independent motion for approvals and questions. */
import { expect, test, type Locator, type Page } from '@playwright/test';
import type { OperatorQuestion } from '../../ui/index.ts';
import { STUB, emit, modelResponse, open } from './harness.ts';

/** Finish the next two animation frames so scroll reconciliation settles. */
async function frames(page: Page): Promise<void> {
    await page.evaluate(() => new Promise<void>(resolve => requestAnimationFrame(() =>
        requestAnimationFrame(() => resolve()))));
}

/** Exercise real scrollbar compensation, including when only one pane overflows. */
test.use({ launchOptions: { ignoreDefaultArgs: ['--hide-scrollbars'] } });

function question(id: string, long = false): OperatorQuestion {
    return {
        question_id: id,
        question: long ? 'UnbrokenQuestion'.repeat(100) : 'Which approach should I use?',
        options: long ? ['UnbrokenOption'.repeat(100), 'A shorter alternative'] : ['First', 'Second'],
        deadline_at: Date.now() + 600_000,
        asked_at: Date.now(),
    };
}

async function questions(page: Page, values: OperatorQuestion[]): Promise<void> {
    await page.request.post(`${STUB}/__stub/message`, { data: {
        type: 'questions', session: 'demo', questions: values,
    } });
}

async function approval(page: Page, id: string, long = false): Promise<void> {
    await page.request.post(`${STUB}/__stub/confirm`, { data: {
        confirmation_id: id, deadline_at: new Date(Date.now() + 600_000).toISOString(),
        call: { name: 'run_command', arguments: { command: long ? 'unbroken-command/'.repeat(100) : 'echo test' } },
    } });
    await expect(page.locator(`[data-confirmation="${id}"]`)).toBeVisible();
}

/** Finish CSS arrivals before comparing layout; no arbitrary timeout is needed. */
async function finishArrival(card: Locator): Promise<void> {
    await card.locator('..').evaluate(async node => {
        await Promise.all(node.getAnimations().map(animation => animation.finished));
    });
}

for (const theme of ['light', 'dark'] as const) {
    for (const viewport of [
        { width: 360, height: 480, inspector: false },
        { width: 768, height: 960, inspector: false },
        { width: 1440, height: 960, inspector: false },
        { width: 1440, height: 960, inspector: true },
    ]) {
        test(`decision surfaces stay inside the reading column: ${theme} ${viewport.width} inspector=${viewport.inspector}`, async ({ page }, testInfo) => {
            await page.setViewportSize(viewport);
            await page.addInitScript(value => localStorage.setItem('simplex.panel.theme', value), theme);
            await open(page, '?session=demo');
            if (viewport.inspector) await page.getByRole('button', { name: 'Show inspector', exact: true }).click();
            await page.addStyleTag({ content: `
                .reading-scroll { overflow-y: scroll !important; }
                .reading-scroll::-webkit-scrollbar, .decisions-shell::-webkit-scrollbar { width: 16px; }
            ` });
            await approval(page, 'bounds', true);
            await questions(page, [question('bounds-question', true)]);
            const cards = page.locator('[data-panel-frame="decision"]');
            await expect(cards).toHaveCount(2);
            for (const card of await cards.all()) await finishArrival(card);

            async function expectContained(): Promise<void> {
                await expect.poll(async () => page.evaluate(() => {
                    const reading = document.querySelector('[data-testid="reading-surface"]')!.getBoundingClientRect();
                    const composer = document.querySelector('[data-testid="composer-surface"]')!.getBoundingClientRect();
                    return [...document.querySelectorAll<HTMLElement>('[data-panel-frame="decision"]')].every(card => {
                        const box = card.getBoundingClientRect();
                        return Math.abs(box.left - reading.left) < 1 && Math.abs(box.width - reading.width) < 1
                            && Math.abs(box.left - composer.left) < 1 && Math.abs(box.width - composer.width) < 1
                            && card.scrollWidth <= card.clientWidth + 1
                            && [...card.querySelectorAll('button, textarea')].every(control => {
                                const bounds = control.getBoundingClientRect();
                                return bounds.left >= box.left && bounds.right <= box.right + 1;
                            });
                    });
                })).toBe(true);
            }

            await expectContained();
            await page.getByTestId('approval-disclosure').click();
            await expect(page.getByTestId('approval-arguments')).toBeVisible();
            await expectContained();
            const region = page.locator('.decisions-shell');
            await expect(region).toHaveCSS('background-color', 'rgba(0, 0, 0, 0)');
            await expect(region).toHaveCSS('backdrop-filter', 'none');
            expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
            await expect(page.getByTestId('composer-surface')).toBeInViewport({ ratio: 1 });
            const answer = page.getByRole('textbox', { name: 'Answer', exact: true });
            await answer.fill('A freely entered answer');
            await expect(answer).toHaveValue('A freely entered answer');
            await expectContained();
            if (viewport.width === 1440 && !viewport.inspector) {
                // Capture ordinary-sized decisions together after the stress checks.
                await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'bounds' } });
                await expect(page.locator('[data-confirmation="bounds"]')).toHaveCount(0);
                await emit(page, 'model_response', modelResponse(
                    'This response stays in place while the operator reviews a request.\n\n'.repeat(12),
                ));
                await approval(page, 'review');
                await questions(page, [question('review-question')]);
                await expect(page.locator('[data-question="bounds-question"]')).toHaveCount(0);
                await expect(page.locator('[data-question="review-question"]')).toBeVisible();
                for (const card of await cards.all()) await finishArrival(card);
                const path = testInfo.outputPath('bounded-decisions.png');
                await page.screenshot({ path, animations: 'disabled' });
                await testInfo.attach('bounded-decisions', {
                    path, contentType: 'image/png',
                });
            }
        });
    }
}

/** Record actual entrance events, including short animations that finish before an assertion. */
type MotionWindow = { decisionAnimations: string[] };

test('each new decision enters once; updates and submissions keep its identity', async ({ page }) => {
    await page.addInitScript(() => {
        (window as unknown as MotionWindow).decisionAnimations = [];
        document.addEventListener('animationstart', event => {
            const node = event.target;
            if (node instanceof HTMLElement && node.matches('.decision-motion')) {
                const card = node.querySelector<HTMLElement>('[data-panel-frame="decision"]')!;
                (window as unknown as MotionWindow).decisionAnimations.push(`${card.dataset.confirmation ?? card.dataset.question}:${event.animationName}`);
            }
        });
    });
    await open(page, '?session=demo');

    await approval(page, 'first');
    const first = page.locator('[data-confirmation="first"]');
    await finishArrival(first);
    const timer = first.locator('.tabular-nums');
    const countdown = await timer.textContent();
    await expect(timer).not.toHaveText(countdown!);
    await approval(page, 'second');
    const firstQuestion = question('question-first');
    await questions(page, [firstQuestion]);
    await expect(page.locator('[data-question="question-first"]')).toBeVisible();
    await expect(page.locator('[data-question="question-first"] .tabular-nums')).toHaveText(/^\d+:\d{2} left$/);
    await questions(page, [firstQuestion, question('question-second')]);
    const cards = page.locator('[data-panel-frame="decision"]');
    await expect(cards).toHaveCount(4);
    for (const card of await cards.all()) {
        await finishArrival(card);
        await expect(card.locator('..')).toHaveCSS('animation-name', 'decision-enter');
    }
    // Neither section nor the region adds a second animation around the cards.
    for (const selector of ['.approvals-shell', '.questions-shell', '.decisions-shell']) {
        await expect(page.locator(selector)).toHaveCSS('animation-name', 'none');
    }
    await first.getByTestId('approval-disclosure').click();
    await first.getByRole('button', { name: 'Approve', exact: true }).click();
    await expect(first.getByRole('button', { name: 'Approve', exact: true })).toBeDisabled();
    await page.locator('[data-question="question-first"]').getByRole('button', { name: 'First', exact: true }).click();
    await questions(page, [{ ...firstQuestion, question: 'The same pending question, refreshed' }, question('question-second')]);
    await emit(page, 'model_response', modelResponse('Unrelated output'));
    await expect(page.locator('[data-question="question-first"]')).toContainText('The same pending question, refreshed');
    await expect(page.getByTestId('transcript')).toContainText('Unrelated output');
    await expect.poll(() => page.evaluate(() => (window as unknown as MotionWindow).decisionAnimations)).toEqual([
        'first:decision-enter', 'second:decision-enter', 'question-first:decision-enter', 'question-second:decision-enter',
    ]);
    // A stable keyed component also preserves its open disclosure across updates.
    await expect(first.getByTestId('approval-disclosure')).toHaveAttribute('aria-expanded', 'true');
    await expect(first.getByRole('button', { name: 'Approve', exact: true })).toBeDisabled();
    await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'first' } });
    await questions(page, [question('question-second')]);
    await expect(cards).toHaveCount(2);
    const animations = await page.evaluate(() => (window as unknown as MotionWindow).decisionAnimations);
    expect(animations.slice(4).sort()).toEqual(['first:decision-exit', 'question-first:decision-exit']);
});

test('reduced motion leaves decisions visible and usable, and settlement leaves no empty strip', async ({ page }) => {
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await open(page, '?session=demo');
    const input = page.getByRole('textbox', { name: 'message', exact: true });
    await input.fill('Keep my draft and focus');
    await approval(page, 'quiet');
    await questions(page, [{ ...question('quiet-question'), options: [] }]);
    const cards = page.locator('[data-panel-frame="decision"]');
    await expect(cards).toHaveCount(2);
    await expect(input).toBeFocused();
    for (const card of await cards.all()) {
        await finishArrival(card);
        const motion = card.locator('..');
        expect(await motion.evaluate(node => Number.parseFloat(getComputedStyle(node).animationDuration))).toBeLessThan(0.05);
        await expect(motion).toHaveCSS('opacity', '1');
        expect(await motion.evaluate(node => {
            const transform = getComputedStyle(node).transform;
            return transform === 'none' || new DOMMatrixReadOnly(transform).isIdentity;
        })).toBe(true);
    }
    await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'quiet' } });
    await questions(page, []);
    await expect(cards).toHaveCount(0);
    await expect.poll(() => page.locator('.decisions-shell').evaluate(node => node.getBoundingClientRect().height)).toBe(0);
    await expect(input).toHaveValue('Keep my draft and focus');
    await expect(input).toBeFocused();
});

for (const readingOlder of [false, true]) {
    test(`decisions leave the transcript geometry fixed while ${readingOlder ? 'reading older messages' : 'following the latest reply'}`, async ({ page }) => {
        await open(page, '?session=demo');
        await emit(page, 'input_admitted', { operation: 'message' });
        await emit(page, 'model_response', modelResponse('A line to read.\n\n'.repeat(120)));
        const scroller = page.getByTestId('transcript');
        await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
        if (readingOlder) {
            await scroller.hover();
            await page.mouse.wheel(0, -600);
            await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
        }

        const geometry = () => page.evaluate(() => {
            const transcript = document.querySelector<HTMLElement>('[data-testid="transcript"]')!;
            return {
                transcript: transcript.getBoundingClientRect().toJSON(),
                scrollTop: transcript.scrollTop,
                scrollHeight: transcript.scrollHeight,
                composer: document.querySelector('[data-testid="composer-surface"]')!.getBoundingClientRect().toJSON(),
            };
        });
        const before = await geometry();
        await approval(page, 'overlay', true);
        await questions(page, [question('overlay-question')]);
        const first = page.locator('[data-confirmation="overlay"]');
        const second = page.locator('[data-question="overlay-question"]');
        await expect(second).toBeVisible();
        await finishArrival(first);
        await finishArrival(second);
        expect(await geometry()).toEqual(before);
        const region = page.locator('.decisions-shell');
        const box = (await region.boundingBox())!;
        expect(box.y).toBe(before.transcript.top);
        expect(box.y + box.height).toBeLessThan(before.transcript.bottom);
        await first.getByTestId('approval-disclosure').click();
        await expect(first.getByTestId('approval-arguments')).toBeVisible();
        expect(await geometry()).toEqual(before);

        // Hold the CSS exit so inert controls and empty-layer hit testing can
        // be observed reliably before the animation finishes.
        const paused = await page.addStyleTag({ content: `
            .decision-motion[data-state="leaving"] { animation-play-state: paused; }
        ` });
        await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'overlay' } });
        await questions(page, []);
        for (const card of [first, second]) {
            await expect(card.locator('..')).toHaveAttribute('inert', '');
            await expect(card.locator('..')).toHaveCSS('animation-name', 'decision-exit');
        }
        await expect(region).toHaveCSS('pointer-events', 'none');
        expect(await geometry()).toEqual(before);
        await paused.evaluate(node => node.parentNode?.removeChild(node));
        await expect(first).toHaveCount(0);
        await expect(second).toHaveCount(0);
        expect(await geometry()).toEqual(before);
        if (readingOlder) {
            await page.getByRole('button', { name: 'jump to latest' }).click();
            await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
        }
    });
}

test('a covered reading control yields focus to the visible pane and settles back', async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 480 });
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse(
        '[Covered link](https://example.com)\n\n' + 'Reading text.\n\n'.repeat(20)));
    const link = page.getByRole('link', { name: 'Covered link' });
    const scroller = page.getByTestId('transcript');
    await scroller.hover();
    await page.mouse.wheel(0, -200);
    await link.scrollIntoViewIfNeeded();
    await link.focus();
    await expect(link).toBeFocused();
    await questions(page, [question('focus-question')]);
    const card = page.locator('[data-question="focus-question"]');
    await expect(card).toBeVisible();
    // The arriving card must not take focus, and the covered link must not keep it.
    await expect(page.getByTestId('transcript')).toBeFocused();
    // Tab skips the covered history and enters the open decision, rather than
    // bouncing back to the same reading surface.
    await page.keyboard.press('Tab');
    expect(await page.evaluate(() =>
        document.activeElement?.closest('[data-question="focus-question"]') !== null)).toBe(true);
    await expect(card.getByRole('textbox', { name: 'Answer' })).not.toBeFocused();
    // A settled card releases focus back to the pane instead of dropping it on body.
    await card.getByRole('textbox', { name: 'Answer' }).focus();
    await questions(page, []);
    await expect(card).toHaveCount(0);
    await expect(page.getByTestId('transcript')).toBeFocused();
});

test('the Plan pane yields focus under a floating decision', async ({ page }) => {
    await open(page, '?session=demo');
    await page.request.post(`${STUB}/__stub/plan`, { data: { plan: {
        markdown: '[Plan link](https://example.com)\n\n' + 'Plan line.\n\n'.repeat(40),
        revision: 1, updated_at: null,
    } } });
    await page.getByRole('tab', { name: 'Plan', exact: true }).click();
    const plan = page.getByTestId('plan-content');
    const link = plan.getByRole('link', { name: 'Plan link' });
    await link.focus();
    await expect(link).toBeFocused();
    await questions(page, [question('plan-focus')]);
    await expect(page.locator('[data-question="plan-focus"]')).toBeVisible();
    await expect(plan).toBeFocused();
});

test('a control below the floating card keeps focus', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse(
        '[Top link](https://example.com/top)\n\n' + 'Reading text.\n\n'.repeat(40)
        + '[Bottom link](https://example.com/bottom)'));
    const bottom = page.getByRole('link', { name: 'Bottom link' });
    await bottom.scrollIntoViewIfNeeded();
    await bottom.focus();
    await questions(page, [question('below-question')]);
    await expect(page.locator('[data-question="below-question"]')).toBeVisible();
    await expect(bottom).toBeFocused();
});

test('a text-only Plan keeps its keyboard entry point', async ({ page }) => {
    await open(page, '?session=demo');
    await page.request.post(`${STUB}/__stub/plan`, { data: { plan: {
        markdown: '# Plan\n\n' + 'Plan text.\n\n'.repeat(40),
        revision: 1, updated_at: null,
    } } });
    await page.getByRole('tab', { name: 'Plan', exact: true }).click();
    await page.keyboard.press('Tab');
    await expect(page.getByTestId('plan-content')).toBeFocused();
});

test('only the visible clipped part of a decision card governs focus', async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 480 });
    await open(page, '?session=demo');
    await page.request.post(`${STUB}/__stub/plan`, { data: { plan: {
        markdown: '# Plan\n\n' + 'Plan body.\n\n'.repeat(30) + '[Below card](https://example.com)',
        revision: 1, updated_at: null,
    } } });
    await page.getByRole('tab', { name: 'Plan', exact: true }).click();
    await questions(page, [question('clipped', true)]);
    const card = page.locator('[data-question="clipped"]');
    await expect(card).toBeVisible();
    const plan = page.getByTestId('plan-content');
    const link = plan.getByRole('link', { name: 'Below card' });
    await link.evaluate(node => node.scrollIntoView({ block: 'end' }));
    const geometry = await page.evaluate(() => {
        const card = document.querySelector('[data-question="clipped"]')!.getBoundingClientRect();
        const shell = document.querySelector('.decisions-shell')!.getBoundingClientRect();
        const link = document.querySelector('[data-testid="plan-content"] a[href="https://example.com"]')!
            .getBoundingClientRect();
        return { cardBottom: card.bottom, shellBottom: shell.bottom, linkTop: link.top };
    });
    // The card's content box extends below its scrollport; this link is visible
    // below the clip and must remain the reader's focus target.
    expect(geometry.cardBottom).toBeGreaterThan(geometry.shellBottom);
    expect(geometry.linkTop).toBeGreaterThan(geometry.shellBottom);
    await link.focus();
    await expect(link).toBeFocused();

    // Scrolling the Plan moves the same link under the visible card. The scroll
    // recheck must move focus to the Plan pane without scrolling it further.
    const delta = Math.max(1, geometry.linkTop - geometry.shellBottom + 12);
    const before = await plan.evaluate(node => node.scrollTop);
    await plan.evaluate((node, amount) => { node.scrollTop += amount; }, delta);
    await frames(page);
    await expect(plan).toBeFocused();
    // Moving focus must not add another scroll: only the requested delta (with
    // normal fractional/clamped scrollTop rounding) may be present.
    const after = await plan.evaluate(node => node.scrollTop);
    expect(Math.abs(after - before - delta)).toBeLessThanOrEqual(2);
});
