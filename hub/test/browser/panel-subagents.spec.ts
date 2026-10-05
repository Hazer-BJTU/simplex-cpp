import { expect, test } from '@playwright/test';
import { open, runningSession, STUB, withSession } from './harness.ts';

const id = 'subagent-8d52c3ab-2f81-4e72-89a1-451a09c7de12';

test('headless entries expose status, policy and global approvals without conversation controls', async ({ page }) => {
    await open(page);
    await withSession(page, {
        ...runningSession(id), kind: 'headless',
        subagent: { parent: 'parent-session', lifecycle: 'ready', policy: 'ask',
            health: 'healthy', reason: 'live identified worker event channel', active: false,
            observed_at: '2026-01-01T00:00:01.000Z' },
    });
    await page.reload();
    await page.getByTestId('session-row').click();
    await expect(page.getByTestId('headless-panel')).toBeVisible();
    await expect(page.getByText('Headless subagent · parent parent-session', { exact: true })).toBeVisible();
    await expect(page.getByLabel('message', { exact: true })).toHaveCount(0);
    await expect(page.getByRole('button', { name: 'Send', exact: true })).toHaveCount(0);
    await expect(page.getByRole('button', { name: 'Model options', exact: true })).toHaveCount(0);
    await expect(page.getByRole('button', { name: 'Start worker', exact: true })).toHaveCount(0);
    await page.getByLabel('Subagent safety policy').selectOption('deny');
    await expect.poll(async () => {
        const response = await page.request.get(`${STUB}/__stub/actions`);
        return (await response.json()).actions;
    }).toContainEqual({ session: id, action: 'subagent-policy', policy: 'deny' });
    const received = (await (await page.request.get(`${STUB}/__stub/received`)).json()).received;
    expect(received.filter((message: { type: string; session?: string }) =>
        message.session === id && ['subscribe', 'input', 'history', 'signal'].includes(message.type))).toEqual([]);
    await page.request.post(`${STUB}/__stub/confirm`, {
        data: { session: id, confirmation_id: 'child-approval', call: { name: 'run_command', arguments: { command: 'echo test' } } },
    });
    await expect(page.getByRole('button', { name: 'Approve', exact: true })).toBeVisible();
    await page.getByRole('button', { name: 'Deny', exact: true }).click();
    await expect.poll(async () => (await (await page.request.get(`${STUB}/__stub/decisions`)).json()).decisions.length).toBe(1);
});
