/**
 * @file the polish stage, in a browser.
 *
 * Four things are checked here that nothing else can check:
 *
 *   - **the theme is applied before paint**, which is why the boot script in
 *     `index.html` exists at all and why it has to be tested with the bundle
 *     blocked — with JavaScript loading normally the module would paper over a
 *     missing script and the flash would come back unnoticed;
 *   - **a whole turn is reachable from the keyboard**, driving nothing with the
 *     mouse: the acceptance condition for this stage is "keyboard completes the
 *     flow", so the test uses `page.keyboard` and nothing else;
 *   - **a 390px viewport is usable**, where the two side columns become drawers;
 *   - **the dialog is where it says it is** — centred and opaque. The panel
 *     spent three stages with a dialog that was neither, because a JSX
 *     attribute written as `attr="a" + \`b\`` type-checks and silently drops
 *     everything after the first line.
 */
import { expect, test, type Page } from '@playwright/test';
import { STUB, emit, open, stoppedSession, withSession } from './harness.ts';

/** What the stub has received over the panel socket, newest last. */
async function received(page: Page): Promise<{ type: string; [key: string]: unknown }[]> {
    const response = await page.request.get(`${STUB}/__stub/received`);
    return (await response.json()).received;
}

/**
 * What the stub has been asked to do over REST.
 *
 * Supervisor actions go through `POST /api/sessions/:id/<action>` rather than
 * the socket, so a test that only read `received` would call a working Start
 * button broken.
 */
async function actions(page: Page): Promise<{ session: string; action: string }[]> {
    const response = await page.request.get(`${STUB}/__stub/actions`);
    return (await response.json()).actions;
}

/** Move focus with Tab until it is on `selector`, or give up loudly. */
async function tabTo(page: Page, selector: string, limit = 40): Promise<void> {
    for (let step = 0; step < limit; step += 1) {
        const hit = await page.evaluate(
            (target) => document.activeElement?.matches(target) ?? false, selector,
        );
        if (hit) return;
        await page.keyboard.press('Tab');
    }
    const landed = await page.evaluate(() => {
        const node = document.activeElement;
        return node ? `${node.tagName}.${(node as HTMLElement).className}` : 'nothing';
    });
    throw new Error(`focus never reached ${selector}; it is on ${landed}`);
}

test.describe('theme', () => {
    test('is applied before the bundle runs', async ({ page }) => {
        // With the bundle blocked, the only thing that can set the attribute is
        // the inline script. The theme flash this prevents is exactly the bug
        // that appears when the script is dropped in favour of doing it in React.
        await page.route('**/assets/*.js', (route) => route.abort());
        await page.addInitScript(() => localStorage.setItem('simplex.panel.theme', 'dark'));
        await page.goto('/');
        await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
        await expect(page.locator('html')).toHaveAttribute('data-theme-preference', 'dark');
    });

    test('cycles, applies immediately, and survives a reload', async ({ page }) => {
        await open(page);
        const html = page.locator('html');
        const toggle = page.getByTestId('theme-toggle');
        await expect(html).toHaveAttribute('data-theme', 'light');

        // The cycle starts at `system`, and this machine asks for light, so the
        // first step lands on `light` — the appearance does not change, the
        // label does ("system (light)" becomes "light"). The second step is the
        // one that turns the panel dark.
        await toggle.click();
        await expect(toggle).toHaveAttribute('data-theme-preference', 'light');
        await toggle.click();
        await expect(html).toHaveAttribute('data-theme', 'dark');
        await expect(toggle).toHaveAttribute('data-theme-preference', 'dark');
        // The colour is not decoration: the token the whole panel reads changed.
        await expect(page.locator('body')).toHaveCSS('background-color', /oklch\(0\.185/);

        await page.reload();
        await expect(page.getByText('connected', { exact: true })).toBeVisible();
        await expect(html).toHaveAttribute('data-theme', 'dark');
        await expect(toggle).toContainText('dark');
    });

    test('follows the operating system while the preference is system', async ({ page }) => {
        await open(page);
        await page.emulateMedia({ colorScheme: 'dark' });
        // `system` is the third step of the cycle, and the default.
        await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
        await expect(page.getByTestId('theme-toggle')).toContainText('system (dark)');

        // A fixed preference wins over the machine.
        await page.getByTestId('theme-toggle').click();   // -> light
        await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
        await page.emulateMedia({ colorScheme: 'dark' });
        await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
    });

    test('survives a browser that refuses to store anything', async ({ page }) => {
        await page.addInitScript(() => {
            Object.defineProperty(window, 'localStorage', {
                get() { throw new Error('blocked by policy'); },
            });
        });
        await open(page);
        await expect(page.getByTestId('theme-toggle')).toBeVisible();
        await page.getByTestId('theme-toggle').click();
        await expect(page.getByTestId('theme-toggle')).toHaveAttribute('data-theme-preference', 'light');
        await page.getByTestId('theme-toggle').click();
        await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
    });
});

test.describe('the keyboard', () => {
    test('selects a session, starts a worker, and sends a message', async ({ page }) => {
        await open(page);
        const row = '[data-testid="session-row"][data-session="demo"]';
        await page.locator(row).waitFor();

        // 1. Reach the session row with Tab and open it with Enter.
        await tabTo(page, row);
        await page.keyboard.press('Enter');
        await expect(page).toHaveURL(/session=demo/);
        await expect(page.getByTestId('session-title')).toContainText('demo');

        // 2. Start the worker from the palette.
        await page.keyboard.press('ControlOrMeta+k');
        await expect(page.getByLabel('filter commands')).toBeFocused();
        await page.keyboard.type('start');
        await page.keyboard.press('Enter');
        await expect(page.getByTestId('palette-command')).toHaveCount(0);
        await expect.poll(async () => (await actions(page))
            .some((entry) => entry.session === 'demo' && entry.action === 'start'))
            .toBe(true);

        // 3. Tab to the composer, type, and send with Enter.
        await tabTo(page, 'textarea[aria-label="message"]');
        await page.keyboard.type('hello from the keyboard');
        await page.keyboard.press('Enter');
        await expect.poll(async () => (await received(page))
            .some((message) => message.type === 'input'
                && JSON.stringify(message).includes('hello from the keyboard')))
            .toBe(true);
        await expect(page.locator('textarea[aria-label="message"]')).toHaveValue('');
    });

    test('reaches every control in the header without a mouse', async ({ page }) => {
        await open(page);
        await page.locator('[data-testid="session-row"][data-session="demo"]').click();
        // Every control in the header is a real button with an accessible name,
        // so each one is reachable and each one can say what it does. The list
        // is in DOM order, because Tab only goes forwards.
        for (const target of [
            '[data-testid="session-primary-action"]',
            'button[aria-label="Show inspector"]',
            'button[aria-label="More actions"]',
        ]) {
            await tabTo(page, target);
        }
        // The overflow menu opens from the keyboard like any other menu.
        await page.keyboard.press('Enter');
        await expect(page.getByRole('menuitem', { name: /Restart/ })).toBeVisible();
    });

    test('shows a focus ring on whatever has focus', async ({ page }) => {
        await open(page);
        await page.locator('[data-testid="session-row"][data-session="demo"]').waitFor();
        await tabTo(page, '[data-testid="session-row"][data-session="demo"]');
        const ring = await page.evaluate(() => {
            const style = getComputedStyle(document.activeElement as Element);
            return {
                width: style.outlineWidth,
                style: style.outlineStyle,
                colour: style.outlineColor,
            };
        });
        expect(Number.parseFloat(ring.width)).toBeGreaterThanOrEqual(2);
        expect(ring.style).not.toBe('none');
    });
});

test.describe('the session column', () => {
    test('can be hidden and shown from the status bar', async ({ page }) => {
        await open(page);
        const list = page.getByTestId('session-list');
        const toggle = page.getByTestId('sessions-desktop-toggle');
        await expect(list).toBeVisible();
        await expect(toggle).toHaveAttribute('aria-expanded', 'true');
        await expect(toggle).toHaveAccessibleName('Hide the session list');
        // The delivery warning belongs to the protocol, not the sidebar.
        await expect(page.getByText('The hub has no delivery acknowledgement')).toHaveCount(0);
        const before = (await page.locator('main').boundingBox())!;

        await toggle.click();
        await expect(list).toBeHidden();
        await expect(toggle).toHaveAttribute('aria-expanded', 'false');
        await expect(toggle).toHaveAccessibleName('Show the session list');
        await expect(toggle).toBeFocused();
        const after = (await page.locator('main').boundingBox())!;
        // The conversation takes the width the column gave up.
        expect(after.x).toBeLessThan(before.x - 100);

        await toggle.click();
        await expect(list).toBeVisible();
        await expect(toggle).toHaveAttribute('aria-expanded', 'true');
        await expect(page.getByTestId('session-row').first()).toBeVisible();
    });
});

test.describe('a narrow screen', () => {
    test.use({ viewport: { width: 390, height: 780 } });

    test('keeps the conversation and puts the columns in drawers', async ({ page }) => {
        await open(page);

        // The sidebar is not a column here.
        await expect(page.getByTestId('session-list')).toBeHidden();
        await expect(page.getByTestId('sessions-desktop-toggle')).toBeHidden();
        const overflow = await page.evaluate(() => (
            document.documentElement.scrollWidth - document.documentElement.clientWidth
        ));
        expect(overflow, 'the page scrolls sideways').toBeLessThanOrEqual(1);

        // It opens as a drawer, and choosing a session closes it again.
        await page.getByTestId('sessions-toggle').click();
        await expect(page.getByTestId('session-list')).toBeVisible();
        await page.locator('[data-testid="session-row"][data-session="demo"]').click();
        await expect(page.getByTestId('session-list')).toBeHidden();
        await expect(page.getByTestId('session-title')).toContainText('demo');

        // And the composer still works.
        await page.locator('textarea[aria-label="message"]').fill('narrow hello');
        await page.getByRole('button', { name: 'Send' }).click();
        await expect.poll(async () => (await received(page))
            .some((message) => message.type === 'input')).toBe(true);

        await emit(page, 'run_started', {});
        const cancelBox = await page.getByRole('button', { name: 'Cancel run' }).boundingBox();
        expect(cancelBox).not.toBeNull();
        expect(cancelBox!.x + cancelBox!.width).toBeLessThanOrEqual(390);
        await expect(page.getByRole('button', { name: 'Continue' })).toHaveCount(0);
    });

    test('closes the drawer with Escape', async ({ page }) => {
        await open(page);
        await page.getByTestId('sessions-toggle').click();
        await expect(page.getByTestId('session-list')).toBeVisible();
        await page.keyboard.press('Escape');
        await expect(page.getByTestId('session-list')).toBeHidden();
    });

    test('shows the context drawer over the conversation', async ({ page }) => {
        await open(page);
        await page.getByTestId('sessions-toggle').click();
        await page.locator('[data-testid="session-row"][data-session="demo"]').click();
        await page.getByRole('button', { name: 'Show inspector' }).click();
        const inspector = page.getByTestId('inspector');
        await expect(inspector).toBeVisible();
        await inspector.getByRole('button', { name: 'Close' }).click();
        await expect(inspector).toHaveCount(0);
    });
});

test.describe('motion and placement', () => {
    test('drops the animation when the reader asks for less', async ({ page }) => {
        await page.emulateMedia({ reducedMotion: 'reduce' });
        await open(page);
        await page.locator('[data-testid="session-row"][data-session="demo"]').click();
        await emit(page, 'run_started', {});
        await page.getByTestId('round').first().waitFor();
        const duration = await page.evaluate(() => {
            const round = document.querySelector('[data-testid="round"]') as Element;
            return getComputedStyle(round).animationDuration;
        });
        expect(Number.parseFloat(duration)).toBeLessThan(0.05);
    });

    test('answers a destructive action inside the session header, not over it', async ({ page }) => {
        await open(page);
        await withSession(page, stoppedSession('demo'));
        await page.reload();
        await page.getByTestId('session-row').click();
        await page.getByRole('button', { name: 'More actions' }).click();
        await page.getByRole('menuitem', { name: /Delete session/ }).click();
        const confirm = page.getByTestId('session-confirm');
        await expect(confirm).toBeVisible();

        // Nothing is covered and nothing floats: the question shares the reading
        // column with the composer, inside the header that asked it.
        await expect(page.getByRole('dialog')).toHaveCount(0);
        const geometry = await page.evaluate(() => {
            const column = document.querySelector('[data-testid="session-confirm"] .reading-width')!;
            const composer = document.querySelector('[data-testid="composer-surface"]')!;
            const box = column.getBoundingClientRect();
            const target = composer.getBoundingClientRect();
            return {
                delta: Math.abs(box.x - target.x) + Math.abs(box.width - target.width),
                background: getComputedStyle(document.querySelector('[data-testid="session-confirm"]')!)
                    .backgroundColor,
            };
        });
        expect(geometry.delta).toBeLessThan(1);
        // The header is the surface; the question does not paint another one.
        expect(geometry.background).toBe('rgba(0, 0, 0, 0)');

        await page.keyboard.press('Escape');
        await expect(confirm).toHaveCount(0);
        // Closing returns focus to the control that asked, not to the document.
        await expect(page.getByRole('button', { name: 'More actions' })).toBeFocused();
    });

    test('a new approval says so in the top bar and draws its rule once', async ({ page }) => {
        await open(page);
        await page.locator('[data-testid="session-row"][data-session="demo"]').click();
        await page.request.post(`${STUB}/__stub/confirm`, {
            data: {
                session: 'demo',
                confirmation_id: 'c-polish',
                call: { name: 'run_command', arguments: { command: 'rm -rf /' } },
            },
        });
        const block = page.getByTestId('approval-block');
        await expect(block).toBeVisible();
        // The old modal was impossible to miss because it covered the page. The
        // replacement is a count in the one bar that is always on screen, plus one
        // pass of the block's own rule.
        await expect(page.getByText('1 approval waiting', { exact: true })).toBeVisible();
        const cue = await block.evaluate(node => {
            const style = getComputedStyle(node);
            return { name: style.animationName, count: style.animationIterationCount };
        });
        expect(cue.name).toBe('approval-arrive');
        expect(cue.count).toBe('1');

        await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'c-polish' } });
        await expect(page.getByText('1 approval waiting', { exact: true })).toHaveCount(0);
    });
});

test.describe('the top bar under pressure', () => {
    for (const width of [360, 390]) {
        test(`keeps its controls and its count inside ${width}px`, async ({ page }) => {
            await page.setViewportSize({ width, height: 720 });
            await open(page, '?session=demo');
            await page.request.post(`${STUB}/__stub/confirm`, {
                data: { session: 'demo', confirmation_id: `bar-${width}` },
            });
            await expect(page.getByTestId('approval-block')).toBeVisible();

            for (const state of ['connected', 'reconnecting'] as const) {
                if (state === 'reconnecting') {
                    await page.request.post(`${STUB}/__stub/down`);
                    await expect(page.getByText('reconnecting', { exact: true })).toBeVisible();
                }
                const geometry = await page.evaluate(() => {
                    const header = document.querySelector('header')!;
                    const row = header.firstElementChild as HTMLElement;
                    const theme = document.querySelector('[data-testid="theme-toggle"]')!.getBoundingClientRect();
                    return {
                        overflow: row.scrollWidth - row.clientWidth,
                        themeRight: Math.round(theme.right),
                        themeHeight: Math.round(theme.height),
                        viewport: window.innerWidth,
                    };
                });
                // Nothing may be clipped: the cluster wraps instead, and the
                // theme control keeps its size on the right.
                expect(geometry.overflow, `${width} ${state}`).toBeLessThanOrEqual(0);
                expect(geometry.themeRight, `${width} ${state}`).toBeLessThanOrEqual(geometry.viewport);
                expect(geometry.themeHeight, `${width} ${state}`).toBeLessThanOrEqual(40);
                await expect(page.locator('header').getByText('1 approval', { exact: true })).toBeVisible();
            }
            await page.request.post(`${STUB}/__stub/up`);
        });
    }
});
