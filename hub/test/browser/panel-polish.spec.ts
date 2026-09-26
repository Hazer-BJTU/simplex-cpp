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
        await page.goto('/app.html');
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
        // Cancel only becomes usable once a run is active, and a disabled
        // button is not in the tab order — so start one.
        await emit(page, 'run_started', {});
        // Every control in the header is a real button with an accessible name,
        // so each one is reachable and each one can say what it does. The list
        // is in DOM order, because Tab only goes forwards.
        for (const target of [
            '[data-testid="session-primary-action"]',
            'button[aria-label="Status"]',
            'button[aria-label="Options"]',
            'button[aria-label="Cancel"]',
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

test.describe('a narrow screen', () => {
    test.use({ viewport: { width: 390, height: 780 } });

    test('keeps the conversation and puts the columns in drawers', async ({ page }) => {
        await open(page);

        // The sidebar is not a column here.
        await expect(page.getByTestId('session-list')).toBeHidden();
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

    test('centres a dialog and gives it a surface', async ({ page }) => {
        await open(page);
        await withSession(page, stoppedSession('demo'));
        await page.reload();
        await page.getByTestId('session-row').click();
        await page.getByRole('button', { name: 'More actions' }).click();
        await page.getByRole('menuitem', { name: /Delete session/ }).click();
        const dialog = page.getByRole('dialog');
        await expect(dialog).toBeVisible();

        const geometry = await page.evaluate(() => {
            const node = document.querySelector('[role="dialog"]') as HTMLElement;
            const box = node.getBoundingClientRect();
            return {
                centre: box.x + box.width / 2,
                viewport: window.innerWidth / 2,
                background: getComputedStyle(node).backgroundColor,
            };
        });
        // The class list this reads is the one the regression broke: without
        // `-translate-x-1/2` the box starts at the centre and runs off the edge.
        expect(Math.abs(geometry.centre - geometry.viewport)).toBeLessThan(4);
        expect(geometry.background).not.toBe('rgba(0, 0, 0, 0)');
        // Entry is a fade, so the surface is legitimately transparent for the
        // first frame or two. What must be true is that it *arrives* — a dialog
        // that stayed at zero would be an invisible modal, which is worse than
        // an unanimated one.
        await expect.poll(async () => Number.parseFloat(await page.evaluate(() => {
            const node = document.querySelector('[role="dialog"]') as HTMLElement;
            return getComputedStyle(node).opacity;
        }))).toBeGreaterThan(0.9);
    });

    test('puts focus on the approval dialog itself, arming nothing', async ({ page }) => {
        await open(page);
        await page.locator('[data-testid="session-row"][data-session="demo"]').click();
        await page.request.post(`${STUB}/__stub/confirm`, {
            data: {
                session: 'demo',
                confirmation_id: 'c-polish',
                call: { name: 'run_command', arguments: { command: 'rm -rf /' } },
            },
        });
        await expect(page.getByRole('dialog')).toBeVisible();

        // Focus is inside the dialog — so a screen reader announces it — but on
        // the container, so Enter decides nothing (defect D16).
        const focused = await page.evaluate(() => ({
            role: document.activeElement?.getAttribute('role'),
            tag: document.activeElement?.tagName,
            isButton: document.activeElement?.tagName === 'BUTTON',
        }));
        expect(focused.isButton).toBe(false);
        expect(focused.role ?? focused.tag).toBeTruthy();

        await page.keyboard.press('Enter');
        await expect(page.getByRole('dialog')).toBeVisible();
        const decisions = await page.request.get(`${STUB}/__stub/decisions`);
        expect((await decisions.json()).decisions).toEqual([]);
    });
});
