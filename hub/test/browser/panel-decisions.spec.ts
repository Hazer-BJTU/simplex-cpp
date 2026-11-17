/** Reading-width surfaces and message-like arrivals for approvals and questions. */
import { expect, test, type Locator, type Page } from '@playwright/test';
import type { OperatorQuestion } from '../../ui/index.ts';
import { STUB, emit, modelResponse, open } from './harness.ts';

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
    await card.evaluate(async node => {
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
                await approval(page, 'review');
                await questions(page, [question('review-question')]);
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
type MotionWindow = { decisionEntries: string[] };

test('each new decision enters once like a message; updates and submissions keep its identity', async ({ page }) => {
    await page.addInitScript(() => {
        (window as unknown as MotionWindow).decisionEntries = [];
        document.addEventListener('animationstart', event => {
            const node = event.target;
            if (node instanceof HTMLElement && node.matches('[data-panel-frame="decision"]')) {
                (window as unknown as MotionWindow).decisionEntries.push(`${node.dataset.confirmation ?? node.dataset.question}:${event.animationName}`);
            }
        });
    });
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('An ordinary message'));
    const line = page.getByTestId('conversation-line').last();
    const messageMotion = await line.evaluate(node => {
        const css = getComputedStyle(node);
        return { name: css.animationName, duration: css.animationDuration, easing: css.animationTimingFunction };
    });

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
        expect(await card.evaluate(node => {
            const css = getComputedStyle(node);
            return { name: css.animationName, duration: css.animationDuration, easing: css.animationTimingFunction };
        })).toEqual(messageMotion);
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
    await expect.poll(() => page.evaluate(() => (window as unknown as MotionWindow).decisionEntries)).toEqual([
        'first:line-enter', 'second:line-enter', 'question-first:line-enter', 'question-second:line-enter',
    ]);
    // A stable keyed component also preserves its open disclosure across updates.
    await expect(first.getByTestId('approval-disclosure')).toHaveAttribute('aria-expanded', 'true');
    await expect(first.getByRole('button', { name: 'Approve', exact: true })).toBeDisabled();
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
        expect(await card.evaluate(node => Number.parseFloat(getComputedStyle(node).animationDuration))).toBeLessThan(0.05);
        await expect(card).toHaveCSS('opacity', '1');
        expect(await card.evaluate(node => {
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
