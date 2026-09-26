/**
 * @file an accessibility audit, measured rather than asserted.
 *
 * Two things here are worth more than a checklist:
 *
 *   - **contrast is computed from the rendered page**, in both themes, by
 *     parsing the computed colours through the browser's own CSS-colour parser
 *     (a canvas `fillStyle`) and applying the WCAG formula. A hand-picked
 *     OKLCH palette is exactly the kind of thing that looks right and measures
 *     wrong, and the token layer makes it cheap to fix in one place;
 *   - **every control has an accessible name**, which is checked over the real
 *     DOM rather than by reading the source: an icon button with a missing
 *     label is invisible to a screen reader and obvious to this.
 *
 * It is deliberately not a general-purpose auditing tool. It checks the panel
 * as it is built, so a token change that ruins the dark theme fails here rather
 * than in someone's eyes.
 */
import { expect, test, type Page } from '@playwright/test';
import { STUB, emit, open } from './harness.ts';

/** Contrast of a selector's text against its own effective background. */
async function ratio(page: Page, selector: string): Promise<{
    fg: string; bg: string; ratio: number; text: string;
}> {
    return page.evaluate((target) => {
        const node = document.querySelector(target);
        if (!node) throw new Error(`no element matches ${target}`);

        /** Any CSS colour to 8-bit sRGB, via the browser's own parser. */
        const canvas = document.createElement('canvas');
        canvas.width = 1;
        canvas.height = 1;
        const context = canvas.getContext('2d')!;
        function parse(value: string): [number, number, number] {
            context.fillStyle = '#000000';
            context.fillStyle = value;
            context.fillRect(0, 0, 1, 1);
            const [r, g, b] = context.getImageData(0, 0, 1, 1).data;
            return [r as number, g as number, b as number];
        }

        /** The first ancestor that actually paints a background. */
        function backgroundOf(element: Element): string {
            let current: Element | null = element;
            while (current) {
                const value = getComputedStyle(current).backgroundColor;
                if (value && !/rgba?\([^)]*,\s*0\)$/.test(value) && value !== 'transparent') {
                    return value;
                }
                current = current.parentElement;
            }
            return 'rgb(255, 255, 255)';
        }

        const luminance = ([r, g, b]: [number, number, number]): number => {
            const channel = (value: number): number => {
                const scaled = value / 255;
                return scaled <= 0.03928 ? scaled / 12.92 : ((scaled + 0.055) / 1.055) ** 2.4;
            };
            return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b);
        };

        const style = getComputedStyle(node);
        const fg = parse(style.color);
        const bg = parse(backgroundOf(node));
        const [light, dark] = [luminance(fg), luminance(bg)].sort((a, b) => b - a) as
            [number, number];
        return {
            fg: style.color,
            bg: backgroundOf(node),
            ratio: Math.round(((light + 0.05) / (dark + 0.05)) * 100) / 100,
            text: (node.textContent ?? '').trim().slice(0, 30),
        };
    }, selector);
}

/** Pairs that must be readable, by selector. */
const TEXT = [
    ['primary text on the panel surface', '[data-testid="session-title"]'],
    ['body text in a response', '[data-testid="assistant-message"] .md p'],
    ['muted metadata', '[data-testid="transcript-stats"]'],
    ['a status pill', '[data-testid="session-row"] span:has-text("worker attached")'],
    ['an empty state', '[data-testid="transcript"] p, [data-testid="sessions-empty"] p'],
];

test.describe('contrast', () => {
    for (const theme of ['light', 'dark'] as const) {
        test(`text is readable in the ${theme} theme`, async ({ page }) => {
            await page.emulateMedia({ colorScheme: theme });
            await open(page);
            await page.locator('[data-testid="session-row"][data-session="demo"]').click();
            await emit(page, 'model_response', {
                type: 'model_response',
                role: 'assistant',
                content: [{ type: 'text', raw: 'A response with some text in it.' }],
            });
            await page.getByTestId('assistant-message').waitFor();
            await expect(page.locator('html')).toHaveAttribute('data-theme', theme);

            const measured: string[] = [];
            for (const [label, selector] of TEXT) {
                const found = await ratio(page, selector as string).catch(() => null);
                if (!found) continue;
                measured.push(`${label}: ${found.ratio}:1 (${found.fg} on ${found.bg})`);
                // WCAG AA for normal text. The panel's smallest text is 12px,
                // which is not "large" by any reading of the guideline.
                expect(found.ratio, `${label} — ${found.text}`).toBeGreaterThanOrEqual(4.5);
            }
            expect(measured.length).toBeGreaterThan(2);
            console.log(`  ${theme}: ${measured.join('\n  ' + theme + ': ')}`);
        });
    }

    test('a destructive control is readable against its own tint', async ({ page }) => {
        await open(page);
        await page.locator('[data-testid="session-row"][data-session="demo"]').click();
        await page.request.post(`${STUB}/__stub/confirm`, {
            data: {
                session: 'demo',
                confirmation_id: 'c-contrast',
                call: { name: 'run_command', arguments: { command: 'ls' } },
            },
        });
        await page.getByTestId('approval-banner').waitFor();
        const measured = await ratio(page, '[data-testid="approval-banner"]');
        expect(measured.ratio).toBeGreaterThanOrEqual(4.5);
    });
});

test('the panel has the structure a screen reader needs', async ({ page }) => {
    await open(page);
    await page.locator('[data-testid="session-row"][data-session="demo"]').click();
    await emit(page, 'model_response', {
        type: 'model_response',
        role: 'assistant',
        content: [{ type: 'text', raw: '## Heading\n\nSome text.' }],
    });
    await page.getByTestId('assistant-message').waitFor();

    const audit = await page.evaluate(() => {
        const visible = (element: Element): boolean => {
            const box = element.getBoundingClientRect();
            return box.width > 0 && box.height > 0;
        };

        // Every control needs a name: text, aria-label, aria-labelledby, or a
        // title. An icon button without one is a button that says nothing.
        const unnamed = [...document.querySelectorAll('button, a[href], input, select, textarea')]
            .filter(visible)
            .filter((element) => {
                if (element.getAttribute('aria-hidden') === 'true') return false;
                const label = element.getAttribute('aria-label')
                    ?? element.getAttribute('title')
                    ?? (element.textContent ?? '').trim()
                    ?? '';
                if (label.length > 0) return false;
                const labelled = element.getAttribute('aria-labelledby');
                if (labelled && document.getElementById(labelled)) return false;
                const id = element.getAttribute('id');
                if (id && document.querySelector(`label[for="${id}"]`)) return false;
                if (element.closest('label')) return false;
                if (element instanceof HTMLInputElement) {
                    // A checkbox inside its own <label> is named by that label.
                    return !element.closest('label');
                }
                return true;
            })
            .map((element) => `${element.tagName}.${(element as HTMLElement).className.slice(0, 40)}`);

        // Landmarks, so the page can be navigated by region.
        const landmarks = {
            main: document.querySelectorAll('main').length,
            navigation: document.querySelectorAll('nav, [role="navigation"]').length,
            labelledAsides: [...document.querySelectorAll('aside')]
                .every((aside) => Boolean(aside.getAttribute('aria-label'))),
        };

        // Heading order: one h1, and no level skipped on the way down.
        const levels = [...document.querySelectorAll('h1, h2, h3, h4, h5, h6')]
            .filter(visible)
            .map((heading) => Number(heading.tagName[1]));
        const skips: string[] = [];
        levels.reduce((previous, level) => {
            if (previous && level > previous + 1) skips.push(`h${previous} -> h${level}`);
            return level;
        }, 0);

        // Icons must be decorative or named, never silent-but-meaningful.
        const icons = [...document.querySelectorAll('svg')].filter(visible);
        const unnamedIcons = icons.filter((icon) => icon.getAttribute('aria-hidden') !== 'true'
            && !icon.getAttribute('aria-label') && !icon.querySelector('title')).length;

        return {
            unnamed,
            landmarks,
            headingCount: levels.filter((level) => level === 1).length,
            skips,
            icons: icons.length,
            unnamedIcons,
        };
    });

    expect(audit.unnamed, 'controls with no accessible name').toEqual([]);
    expect(audit.landmarks.main).toBe(1);
    expect(audit.landmarks.labelledAsides, 'an <aside> with no label').toBe(true);
    expect(audit.headingCount, 'exactly one h1').toBe(1);
    expect(audit.skips, 'heading levels skipped').toEqual([]);
    expect(audit.icons).toBeGreaterThan(3);
    expect(audit.unnamedIcons, 'a visible icon that says nothing').toBe(0);
});
