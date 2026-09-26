/**
 * @file panel shell smoke test.
 *
 * The claim under test is narrow and worth stating: the browser bundle builds,
 * loads, renders, and reads its protocol constants from `shared/protocol.ts` —
 * the same file the Node server reads. That last part is the whole point of the
 * shared module, and it is the one thing a bundler can silently get wrong by
 * resolving a second copy.
 *
 * The transcript, composer and inspector are tested as they land; this file
 * exists so that a broken build fails here rather than in a browser someone
 * opened by hand.
 */
import { expect, test } from '@playwright/test';

test('the panel shell renders the shared protocol constants', async ({ page }) => {
    await page.goto('/app.html');

    await expect(page.getByRole('heading', { name: 'simplex hub panel' })).toBeVisible();
    await expect(page.getByText('simplex-hub-panel v1')).toBeVisible();

    // A capability the hub advertises, rendered from the shared list.
    await expect(page.getByText('transcript-replay')).toBeVisible();
});

test('the page reports no console errors while loading', async ({ page }) => {
    const errors: string[] = [];
    page.on('console', (message) => {
        if (message.type() === 'error') errors.push(message.text());
    });
    page.on('pageerror', (error) => errors.push(error.message));

    await page.goto('/app.html');
    await expect(page.getByRole('heading', { name: 'simplex hub panel' })).toBeVisible();

    expect(errors).toEqual([]);
});
