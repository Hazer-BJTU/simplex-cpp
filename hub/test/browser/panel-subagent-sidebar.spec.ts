import { expect, test } from '@playwright/test';
import { STUB, open, setSessions } from './harness.ts';

test('subagents stay separate, expose approvals while folded, and navigate to their family', async ({ page }) => {
    await open(page);
    await setSessions(page, ['primary', 'child', 'nested', 'other']);
    const response = await page.request.get(`${STUB}/api/sessions`);
    const { sessions } = await response.json();
    for (const session of sessions) {
        if (!['child', 'nested'].includes(session.session_id)) continue;
        session.kind = 'headless';
        session.subagent = {
            parent: session.session_id === 'child' ? 'primary' : 'child',
            lifecycle: 'ready', policy: 'ask', health: 'healthy', active: false,
        };
        session.confirmations = [];
    }
    await page.request.post(`${STUB}/__stub/sessions`, { data: { sessions } });
    await page.getByRole('button', { name: 'Refresh the session list', exact: true }).click();
    const mains = page.getByTestId('main-sessions');
    await expect(mains.locator('[data-session="primary"]')).toBeVisible();
    await expect(mains.locator('[data-session="child"]')).toHaveCount(0);
    const toggle = page.getByRole('button', { name: /Subagents · 2/ });
    await expect(toggle).toHaveAttribute('aria-expanded', 'false');
    await page.getByRole('button', { name: 'Show subagents of primary', exact: true }).click();
    await expect(toggle).toHaveAttribute('aria-expanded', 'true');
    const family = page.locator('[data-family="primary"]');
    await expect(family).toBeFocused();
    await expect(family.locator('[data-session="nested"]')).toBeVisible();
    await family.locator('[data-session="nested"]').click();
    await expect(page.getByTestId('headless-panel')).toBeVisible();
    await expect(page.getByRole('combobox', { name: 'Subagent safety policy' })).toBeVisible();
    await toggle.click();
    // Updating approval metadata must not reopen or steal focus from the toggle.
    sessions.find((item: { session_id: string }) => item.session_id === 'nested').confirmations = [{
        settled_at: null,
    }];
    await page.request.post(`${STUB}/__stub/sessions`, { data: { sessions } });
    await page.getByRole('button', { name: 'Refresh the session list', exact: true }).click();
    await expect(toggle).toHaveAttribute('aria-expanded', 'false');
    await expect(toggle).toContainText('1 awaiting approval');
    await expect(page.getByRole('button', { name: 'Show subagents of primary', exact: true })).toContainText('1 awaiting approval');
});

test('large families scroll independently without pushing main sessions out of view', async ({ page }) => {
    await open(page);
    await setSessions(page, ['primary', ...Array.from({ length: 30 }, (_, index) => `child-${index}`)]);
    const { sessions } = await (await page.request.get(`${STUB}/api/sessions`)).json();
    for (const session of sessions) {
        if (session.session_id === 'primary') continue;
        session.kind = 'headless';
        session.subagent = { parent: 'primary', lifecycle: 'ready', policy: 'ask' };
    }
    await page.request.post(`${STUB}/__stub/sessions`, { data: { sessions } });
    await page.getByRole('button', { name: 'Refresh the session list', exact: true }).click();
    await page.getByRole('button', { name: 'Show subagents of primary', exact: true }).click();
    const sidebar = page.getByTestId('session-list');
    const family = page.locator('#subagent-families');
    const sidebarBox = await sidebar.boundingBox();
    const sectionBox = await page.getByRole('region', { name: 'Subagents', exact: true }).boundingBox();
    expect(sectionBox!.height).toBeLessThanOrEqual(sidebarBox!.height * 0.45 + 1);
    expect(await family.evaluate(element => element.scrollHeight > element.clientHeight)).toBe(true);
    await family.evaluate(element => { element.scrollTop = element.scrollHeight; });
    await expect(page.locator('[data-session="primary"]')).toBeInViewport();
    await page.setViewportSize({ width: 390, height: 720 });
    // The same section is used in the mobile drawer.
    await page.getByRole('button', { name: 'Open the session list', exact: true }).click();
    await expect(sidebar).toBeVisible();
    expect(await sidebar.evaluate(element => element.scrollWidth <= element.clientWidth)).toBe(true);
});
