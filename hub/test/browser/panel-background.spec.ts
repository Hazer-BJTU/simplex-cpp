import { test, expect, type Page } from '@playwright/test';
import { createHash } from 'node:crypto';
import { backgroundPng } from '../helpers/background-image.ts';
import { STUB, emit, modelResponse } from './harness.ts';

const bytes = backgroundPng([240, 60, 40]);
const descriptor = { url: '/api/panel/background/image', revision: createHash('sha256').update(bytes).digest('hex'),
    size: 'cover', position: 'center', opacity: { light: 0.12, dark: 0.08 } };

async function setup(page: Page, options: Partial<typeof descriptor> = {}, status = 200) {
    await page.request.post(`${STUB}/__stub/reset`);
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
