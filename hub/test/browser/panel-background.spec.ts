import { test, expect, type Page } from '@playwright/test';
import { createHash } from 'node:crypto';
import { backgroundPng } from '../helpers/background-image.ts';
import { STUB, call, emit, modelResponse, toolResult } from './harness.ts';

const bytes = backgroundPng([240, 60, 40]);
const descriptor = { url: '/api/panel/background/image', revision: createHash('sha256').update(bytes).digest('hex'),
    size: 'cover', position: 'center', opacity: { light: 0.12, dark: 0.08 },
    toolCardOpacity: { light: 0.6, dark: 0.6 }, frameOpacity: { light: 0.3, dark: 0.45 } };

/** Metadata overrides; `undefined` on a field models a Hub that omits it. */
type BackgroundOverrides = {
    size?: string;
    position?: string;
    opacity?: { light: number; dark: number } | number;
    toolCardOpacity?: { light: number; dark: number } | number;
    frameOpacity?: { light: number; dark: number } | number | undefined;
};

async function setup(page: Page, options: BackgroundOverrides = {}, status = 200) {
    await page.request.post(`${STUB}/__stub/reset`);
    // JSON drops an undefined field, which is exactly an older Hub's payload.
    await page.request.post(`${STUB}/__stub/settings`, { data: { background: { ...descriptor, ...options } } });
    let requests = 0;
    await page.route('**/api/panel/background/image', async route => {
        requests++;
        await route.fulfill({ status, contentType: 'image/png', body: bytes });
    });
    await page.addInitScript(() => {
        const create = URL.createObjectURL.bind(URL), revoke = URL.revokeObjectURL.bind(URL);
        const active = new Set<string>();
        (window as unknown as { backgroundUrls: Set<string> }).backgroundUrls = active;
        URL.createObjectURL = blob => { const url = create(blob); active.add(url); return url; };
        URL.revokeObjectURL = url => { active.delete(url); revoke(url); };
    });
    await page.goto('/');
    await expect(page.getByText('connected', { exact: true })).toBeVisible();
    return () => requests;
}

/** Alpha of an element's painted background, through the browser's own parser. */
async function backgroundAlpha(page: Page, selector: string): Promise<number> {
    return page.locator(selector).first().evaluate((node) => {
        const canvas = document.createElement('canvas');
        canvas.width = 1;
        canvas.height = 1;
        const context = canvas.getContext('2d')!;
        context.clearRect(0, 0, 1, 1);
        context.fillStyle = getComputedStyle(node).backgroundColor;
        context.fillRect(0, 0, 1, 1);
        return context.getImageData(0, 0, 1, 1).data[3]! / 255;
    });
}

test('one full-page static image follows system theme without fetching on transcript updates', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'light' });
    const count = await setup(page);
    await page.getByRole('button', { name: 'demo worker attached no process' }).click();
    const image = page.getByTestId('panel-background');
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'light dark');
    await expect(image).toHaveCSS('opacity', '0.12');
    await expect(image).toHaveCSS('pointer-events', 'none');
    await expect(image).toHaveCSS('position', 'fixed');
    await expect(image).toHaveCSS('background-repeat', 'no-repeat');
    const initialRequests = count();
    const initial = await image.getAttribute('style');
    for (let i = 0; i < 8; i++) await emit(page, 'model_response', modelResponse(`Answer ${i}`));
    await page.emulateMedia({ colorScheme: 'dark' });
    await expect(image).toHaveCSS('opacity', '0.08');
    expect(count()).toBe(initialRequests);
    expect(await image.getAttribute('style')).toBe(initial);
    expect(await page.evaluate(() => (window as unknown as { backgroundUrls: Set<string> }).backgroundUrls.size)).toBe(1);
    await expect(page.locator('.panel-shell')).toHaveCSS('opacity', '1');
    await expect(page.locator('.panel-shell')).toHaveCSS('background-color', 'rgba(0, 0, 0, 0)');
    await page.request.post(`${STUB}/__stub/plan`, { data: { session: 'demo',
        plan: { markdown: '- Test background', revision: 1, updated_at: new Date().toISOString() } } });
    await page.getByRole('tab', { name: 'Plan', exact: true }).click();
    expect(await image.getAttribute('style')).toBe(initial);
    await page.setViewportSize({ width: 390, height: 720 });
    const bounds = await image.boundingBox();
    expect(bounds).toEqual({ x: 0, y: 0, width: 390, height: 720 });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
});

test('the conversation and composer show the image while the frame keeps a veil', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'light' });
    await setup(page);
    await page.getByTestId('session-row').click();
    await page.getByRole('button', { name: 'Show inspector', exact: true }).click();
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'light dark');

    // The conversation column and the composer let the image through.
    for (const selector of ['.panel-page-surface', '[data-testid="reading-surface"]',
        '.composer-shell', '.composer-surface', '.composer-footer']) {
        expect(await backgroundAlpha(page, selector), selector).toBe(0);
    }
    // The frame is frosted glass: a veil of its own surface colour, so the image
    // reads through it without its detail competing with the labels. Every
    // region that owns the image is checked, not just the status bar.
    const frame = ['header[data-panel-frame="surface"]', '#session-list',
        'main div[data-panel-frame="surface"]', '[data-testid="inspector"]'];
    for (const selector of frame) {
        expect(await backgroundAlpha(page, selector), selector).toBeCloseTo(0.3, 2);
    }
    // Each region keeps its own base, so the session list stays a step below the
    // status bar instead of lifting to the page surface. Custom properties
    // resolve before they are read, so the comparison runs through the browser's
    // colour parser rather than through the token text.
    const bases = await page.evaluate((selectors) => {
        const canvas = document.createElement('canvas');
        canvas.width = 1;
        canvas.height = 1;
        const context = canvas.getContext('2d')!;
        const rgb = (value: string) => {
            context.fillStyle = '#000000';
            context.fillStyle = value;
            context.fillRect(0, 0, 1, 1);
            return Array.from(context.getImageData(0, 0, 1, 1).data).slice(0, 3);
        };
        const root = getComputedStyle(document.documentElement);
        return {
            regions: selectors.map(selector => rgb(getComputedStyle(document.querySelector(selector)!)
                .getPropertyValue('--panel-frame-base'))),
            surface: rgb(root.getPropertyValue('--surface')),
            sunken: rgb(root.getPropertyValue('--sunken')),
        };
    }, frame);
    expect(bases.regions).toEqual([bases.surface, bases.sunken, bases.surface, bases.surface]);

    // A bubble and a tool card keep their grouping with a 60% tint.
    await page.getByLabel('message').fill('A message that keeps its bubble');
    await page.getByRole('button', { name: 'Send' }).click();
    await expect(page.getByTestId('outbox-item')).toBeVisible();
    expect(await backgroundAlpha(page, '[data-testid="outbox-item"]')).toBeCloseTo(0.6, 2);

    await emit(page, 'run_started', {});
    await emit(page, 'tool_calls', [call('call-1', 'run_command', { command: 'echo hi' })]);
    await emit(page, 'tool_results', [toolResult('call-1', 'run_command', 'hi\n')]);
    await emit(page, 'run_finished', { status: 'completed', exchanges: 1 });
    await expect(page.getByTestId('tool-card')).toBeVisible();
    expect(await backgroundAlpha(page, '[data-testid="tool-card"]')).toBeCloseTo(0.6, 2);

    // The decision region lets the image through like the composer, but an
    // approval keeps almost all of its own surface: its contrast must not depend
    // on whatever image the operator chose.
    await page.request.post(`${STUB}/__stub/confirm`, { data: { session: 'demo',
        confirmation_id: 'bg-approval',
        call: { name: 'run_command', arguments: { command: 'echo hi' } } } });
    await expect(page.getByTestId('approval-block')).toBeVisible();
    expect(await backgroundAlpha(page, '.decisions-shell')).toBe(0);
    expect(await backgroundAlpha(page, '.approval-block')).toBeGreaterThanOrEqual(0.9);
});

test('frameOpacity one restores the solid frame and zero leaves only the blur', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'light' });
    await setup(page, { frameOpacity: { light: 1, dark: 0 } });
    await page.getByTestId('session-row').click();
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'light dark');
    const header = page.locator('header').first();
    expect(await backgroundAlpha(page, 'header')).toBe(1);
    await expect(header).toHaveCSS('backdrop-filter', 'blur(0px)');
    await page.emulateMedia({ colorScheme: 'dark' });
    await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
    expect(await backgroundAlpha(page, 'header')).toBe(0);
    await expect(header).toHaveCSS('backdrop-filter', 'blur(24px)');
});

test('a Hub that omits frameOpacity keeps the solid frame', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'light' });
    await setup(page, { frameOpacity: undefined });
    await page.getByTestId('session-row').click();
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'light dark');
    expect(await backgroundAlpha(page, 'header')).toBe(1);
    await expect(page.locator('header').first()).toHaveCSS('backdrop-filter', 'blur(0px)');
});

test('frameOpacity accepts one number for both themes', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'light' });
    await setup(page, { frameOpacity: 0.2 });
    await page.getByTestId('session-row').click();
    expect(await backgroundAlpha(page, 'header')).toBeCloseTo(0.2, 2);
    await page.emulateMedia({ colorScheme: 'dark' });
    await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
    expect(await backgroundAlpha(page, 'header')).toBeCloseTo(0.2, 2);
});

test('frame text keeps WCAG AA over black and white wallpaper at the defaults', async ({ page }) => {
    await setup(page);
    for (const theme of ['light', 'dark'] as const) {
        await page.emulateMedia({ colorScheme: theme });
        await expect(page.locator('html')).toHaveAttribute('data-theme', theme);
        // The frame's worst case: the wallpaper is pure black on one edge of the
        // image and pure white on the other, at the configured image opacity, and
        // the veil is the one theme value the panel actually carries.
        const worst = await page.evaluate(() => {
            const canvas = document.createElement('canvas');
            canvas.width = 1;
            canvas.height = 1;
            const context = canvas.getContext('2d')!;
            const parse = (value: string): [number, number, number] => {
                context.fillStyle = '#000000';
                context.fillStyle = value;
                context.fillRect(0, 0, 1, 1);
                const [r, g, b] = context.getImageData(0, 0, 1, 1).data;
                return [r!, g!, b!];
            };
            const luminance = ([r, g, b]: [number, number, number]) => {
                const channel = (value: number) => {
                    const scaled = value / 255;
                    return scaled <= 0.03928 ? scaled / 12.92 : ((scaled + 0.055) / 1.055) ** 2.4;
                };
                return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b);
            };
            const ratio = (foreground: [number, number, number], background: [number, number, number]) => {
                const [light, dark] = [luminance(foreground), luminance(background)]
                    .sort((a, b) => b - a) as [number, number];
                return (light + 0.05) / (dark + 0.05);
            };
            const root = getComputedStyle(document.documentElement);
            const frame = getComputedStyle(document.querySelector('[data-panel-frame]')!);
            const app = parse(root.getPropertyValue('--app'));
            const surface = parse(root.getPropertyValue('--surface'));
            const dark = document.documentElement.dataset.theme === 'dark';
            const image = Number(root.getPropertyValue(dark
                ? '--panel-image-dark-opacity' : '--panel-image-light-opacity'));
            const veil = Number(frame.getPropertyValue('--panel-frame-opacity'));
            const worstOf = (foreground: [number, number, number]) => Math.min(...[0, 255].map(wall => {
                const backdrop = surface.map((channel, index) =>
                    veil * channel + (1 - veil) * (image * wall + (1 - image) * app[index]!)) as
                    [number, number, number];
                return ratio(foreground, backdrop);
            }));
            return {
                ink: worstOf(parse(root.getPropertyValue('--ink'))),
                muted: worstOf(parse(root.getPropertyValue('--ink-muted'))),
                faint: worstOf(parse(frame.getPropertyValue('--ink-faint'))),
                faintIsMuted: frame.getPropertyValue('--ink-faint').trim()
                    === 'var(--ink-muted)' || parse(frame.getPropertyValue('--ink-faint'))
                        .every((value, index) => value === parse(root.getPropertyValue('--ink-muted'))[index]),
            };
        });
        expect(worst.ink, `${theme} ink`).toBeGreaterThanOrEqual(4.5);
        expect(worst.muted, `${theme} muted`).toBeGreaterThanOrEqual(4.5);
        expect(worst.faint, `${theme} faint`).toBeGreaterThanOrEqual(4.5);
        // The 12px labels take the muted tone inside the frame; over a veil the
        // faint one measured 3.9:1 (light) and 4.4:1 (dark).
        expect(worst.faintIsMuted, `${theme} faint remap`).toBe(true);
    }
});

test('tool cards follow toolCardOpacity in each theme', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'light' });
    await setup(page, { toolCardOpacity: { light: 0.25, dark: 0.8 } });
    await page.getByTestId('session-row').click();
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'light dark');
    await emit(page, 'run_started', {});
    await emit(page, 'tool_calls', [call('call-1', 'run_command', { command: 'echo hi' })]);
    await emit(page, 'tool_results', [toolResult('call-1', 'run_command', 'hi\n')]);
    await expect(page.getByTestId('tool-card')).toBeVisible();
    expect(await backgroundAlpha(page, '[data-testid="tool-card"]')).toBeCloseTo(0.25, 2);
    await page.emulateMedia({ colorScheme: 'dark' });
    await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
    expect(await backgroundAlpha(page, '[data-testid="tool-card"]')).toBeCloseTo(0.8, 2);
});

test('markdown bodies are transparent while headers and labels follow toolCardOpacity', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'light' });
    await setup(page, { toolCardOpacity: { light: 0.25, dark: 0.8 } });
    await page.getByTestId('session-row').click();
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'light dark');
    await emit(page, 'model_response', modelResponse([
        '| Name | Value |', '| --- | --- |', '| alpha | 1 |', '',
        'Inline `code` and a [link](https://example.com).', '',
        '```js', 'const answer = 42;', '```',
    ].join('\n')));
    await expect(page.locator('.md table').first()).toBeVisible();
    await expect(page.locator('.md pre').first()).toBeVisible();
    // Headers and label-like chips share the tool card tint.
    expect(await backgroundAlpha(page, '.md table th')).toBeCloseTo(0.25, 2);
    expect(await backgroundAlpha(page, '.md :not(pre) > code')).toBeCloseTo(0.25, 2);
    expect(await backgroundAlpha(page, '.md .bg-sunken')).toBeCloseTo(0.25, 2);
    // The code body itself shows the image.
    expect(await backgroundAlpha(page, '.md pre')).toBe(0);
    await page.emulateMedia({ colorScheme: 'dark' });
    await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
    expect(await backgroundAlpha(page, '.md table th')).toBeCloseTo(0.8, 2);
});

test('a theme without a visible image keeps the solid conversation surfaces', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'light' });
    await setup(page, { opacity: { light: 0, dark: 0.3 } });
    await page.getByTestId('session-row').click();
    await expect(page.locator('html')).not.toHaveAttribute('data-panel-background', /light/);
    // Without a visible image the page surface is the painted background, so the
    // reading column has nothing to let through.
    expect(await backgroundAlpha(page, '.panel-page-surface')).toBe(1);
    await page.emulateMedia({ colorScheme: 'dark' });
    await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'dark');
    expect(await backgroundAlpha(page, '.panel-page-surface')).toBe(0);
});

test('body text keeps WCAG AA over white and black image regions at 0.3 opacity', async ({ page }) => {
    await setup(page, { opacity: { light: 0.3, dark: 0.3 } });
    for (const theme of ['light', 'dark'] as const) {
        await page.emulateMedia({ colorScheme: theme });
        await expect(page.locator('html')).toHaveAttribute('data-theme', theme);
        // Secondary metadata is exempt by design; deployment/hub.md states the
        // tradeoff, so only the primary ink colour is asserted here.
        const worst = await page.evaluate(() => {
            const canvas = document.createElement('canvas');
            canvas.width = 1;
            canvas.height = 1;
            const context = canvas.getContext('2d')!;
            const parse = (value: string): [number, number, number] => {
                context.fillStyle = '#000000';
                context.fillStyle = value;
                context.fillRect(0, 0, 1, 1);
                const [r, g, b] = context.getImageData(0, 0, 1, 1).data;
                return [r!, g!, b!];
            };
            const luminance = ([r, g, b]: [number, number, number]) => {
                const channel = (value: number) => {
                    const scaled = value / 255;
                    return scaled <= 0.03928 ? scaled / 12.92 : ((scaled + 0.055) / 1.055) ** 2.4;
                };
                return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b);
            };
            const style = getComputedStyle(document.documentElement);
            const ink = parse(style.getPropertyValue('--ink'));
            const app = parse(style.getPropertyValue('--app'));
            // The image layer is the only thing between the app colour and the
            // text: 30% of a pure white or pure black region is the worst case.
            return Math.min(...[0, 255].map(value => {
                const background = app.map(channel => channel * 0.7 + value * 0.3) as
                    [number, number, number];
                const [light, dark] = [luminance(ink), luminance(background)]
                    .sort((a, b) => b - a) as [number, number];
                return (light + 0.05) / (dark + 0.05);
            }));
        });
        expect(worst).toBeGreaterThanOrEqual(4.5);
    }
});

test('explicit image size/alignment and zero-opacity theme keep the opaque fallback', async ({ page }) => {
    await page.emulateMedia({ colorScheme: 'light' });
    await setup(page, { size: '80% auto', position: 'bottom right', opacity: { light: 0, dark: 1 } });
    const image = page.getByTestId('panel-background');
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'dark');
    await expect(image).toHaveCSS('background-size', '80% auto');
    await expect(image).toHaveCSS('background-position', '100% 100%');
    await expect(image).toHaveCSS('opacity', '0');
    expect(await page.locator('.panel-shell').evaluate(el => getComputedStyle(el).backgroundColor)).not.toBe('rgba(0, 0, 0, 0)');
    await page.emulateMedia({ colorScheme: 'dark' });
    await expect(image).toHaveCSS('opacity', '1');
    await page.getByRole('button', { name: 'Create a session', exact: true }).click();
    await expect(page.getByRole('dialog')).toBeVisible();
    await page.getByRole('dialog').getByRole('button', { name: 'Cancel', exact: true }).click();
    await expect(page.getByRole('dialog')).not.toBeVisible();
});

for (const failure of ['http', 'decode', 'disabled'] as const) {
    test(`${failure} leaves the normal panel intact`, async ({ page }) => {
        const count = await setup(page, failure === 'disabled' ? { opacity: { light: 0, dark: 0 } } : {}, failure === 'http' ? 404 : 200);
        if (failure === 'decode') {
            await page.route('**/api/panel/background/image', route => route.fulfill({ contentType: 'image/png', body: 'invalid raster' }));
            await page.reload();
            await expect(page.getByText('connected', { exact: true })).toBeVisible();
        }
        await expect(page.locator('html')).not.toHaveAttribute('data-panel-background');
        await expect(page.getByTestId('panel-background')).toHaveCSS('background-image', 'none');
        if (failure === 'disabled') expect(count()).toBe(0);
    });
}

test('disconnect revokes the image and reconnect installs one fresh allocation', async ({ page }) => {
    await setup(page);
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'light dark');
    await page.request.post(`${STUB}/__stub/down`);
    await expect(page.locator('html')).not.toHaveAttribute('data-panel-background');
    expect(await page.evaluate(() => (window as unknown as { backgroundUrls: Set<string> }).backgroundUrls.size)).toBe(0);
    await page.request.post(`${STUB}/__stub/up`);
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'light dark');
    expect(await page.evaluate(() => (window as unknown as { backgroundUrls: Set<string> }).backgroundUrls.size)).toBe(1);
});

test('an unauthorized image response never leaves a decorative allocation', async ({ page }) => {
    await setup(page, {}, 401);
    await expect(page.getByTestId('panel-background')).toHaveCSS('background-image', 'none');
    await expect(page.locator('html')).not.toHaveAttribute('data-panel-background');
    expect(await page.evaluate(() => (window as unknown as { backgroundUrls: Set<string> }).backgroundUrls.size)).toBe(0);
});

test('a delayed image cannot reappear after the panel disconnects', async ({ page }) => {
    await setup(page);
    await expect(page.locator('html')).toHaveAttribute('data-panel-background', 'light dark');
    let started = false;
    let release!: () => void;
    const gate = new Promise<void>(resolve => { release = resolve; });
    await page.route('**/api/panel/background/image', async route => {
        started = true;
        await gate;
        await route.fulfill({ contentType: 'image/png', body: bytes }).catch(() => {});
    });
    await page.request.post(`${STUB}/__stub/restart`);
    await expect.poll(() => started).toBe(true);
    await page.request.post(`${STUB}/__stub/down`);
    await expect(page.locator('html')).not.toHaveAttribute('data-panel-background');
    release();
    await expect(page.getByTestId('panel-background')).toHaveCSS('background-image', 'none');
    expect(await page.evaluate(() => (window as unknown as { backgroundUrls: Set<string> }).backgroundUrls.size)).toBe(0);
    await page.request.post(`${STUB}/__stub/up`);
});
