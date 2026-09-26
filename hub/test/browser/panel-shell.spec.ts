/**
 * @file the panel in a real browser, against a scripted hub.
 *
 * These are the claims a unit test cannot make: that the bundle builds and
 * loads, that the panel establishes a WebSocket through the proxy and renders
 * what arrives on it, and that a reconnect does not destroy what the operator
 * was reading. The last one is defect A2, and it is the reason this suite
 * exists rather than being left to the store's own tests — the *wiring* between
 * socket, client, store and component is what broke, not any single piece.
 *
 * The hub is `stub-hub.mjs`; it can be told to emit, confirm and restart, which
 * a real hub cannot be asked to do on cue. What lands on the page is checked in
 * `panel-transcript.spec.ts`.
 */
import { expect, test, type Page } from '@playwright/test';
import { STUB, emit, modelResponse, open, setSessions } from './harness.ts';

/** One complete model response with the given text. */
function answer(text: string) {
    return modelResponse(text);
}

/** How many assistant messages are on the page. */
async function answers(page: Page): Promise<number> {
    return page.getByTestId('assistant-message').count();
}

test('the shell renders and lists the hub\'s sessions', async ({ page }) => {
    await open(page);

    await expect(page.getByRole('heading', { name: 'simplex hub' })).toBeVisible();
    await expect(page.getByTestId('session-row')).toHaveCount(1);
    await expect(page.getByText('No session selected')).toBeVisible();
});

test('selecting a session shows what the hub replays', async ({ page }) => {
    await open(page);
    await emit(page, 'model_response', answer('replayed history'));
    await emit(page, 'model_response', answer('and one more'));

    await page.getByTestId('session-row').click();

    await expect(page.getByTestId('transcript')).toContainText('replayed history');
    await expect(page.getByTestId('transcript')).toContainText('and one more');
});

test('a reconnecting panel keeps the transcript it already had (A2)', async ({ page }) => {
    await open(page);
    await emit(page, 'model_response', answer('first answer'));
    await page.getByTestId('session-row').click();
    await expect(page.getByTestId('transcript')).toContainText('first answer');

    await emit(page, 'model_response', answer('second answer'));
    await emit(page, 'model_response', answer('third answer'));
    await expect(page.getByTestId('transcript')).toContainText('third answer');

    const before = await answers(page);
    expect(before).toBeGreaterThanOrEqual(3);

    // The stub closes every panel socket and comes back with a new transcript
    // epoch and an empty transcript — which is both halves of A2 at once: the
    // reconnect itself, and the restart that makes the old cursor meaningless.
    await page.request.post(`${STUB}/__stub/restart`, { data: { epoch: 'stub-epoch-2' } });
    await expect(page.getByText('connected', { exact: true })).toBeVisible();
    await expect(page.getByTestId('transcript-note')).toContainText('hub restarted');

    // The new hub process has nothing to replay, so everything on screen is
    // what the panel kept. This is the assertion A2 is about.
    expect(await answers(page), 'the transcript was cleared by the reconnect')
        .toBeGreaterThanOrEqual(before);
    await expect(page.getByTestId('transcript')).toContainText('first answer');
    await expect(page.getByTestId('transcript')).toContainText('third answer');

    // And the new process's numbering does not collide with the old one's: the
    // envelope it sends is numbered 1 again, and both are on screen.
    await emit(page, 'model_response', answer('after the restart'));
    await expect(page.getByTestId('transcript')).toContainText('after the restart');
    await expect(page.getByTestId('transcript')).toContainText('first answer');
});

test('an approval for a session the panel is not watching still arrives (A1)', async ({ page }) => {
    await page.request.post(`${STUB}/__stub/reset`);
    await setSessions(page, ['watched', 'other']);
    await page.goto('/app.html');
    await expect(page.getByText('connected', { exact: true })).toBeVisible();

    await page.getByTestId('session-row').filter({ hasText: 'watched' }).click();
    await expect(page.getByText('No session selected')).toHaveCount(0);

    const response = await page.request.post(`${STUB}/__stub/confirm`, {
        data: { session: 'other', confirmation_id: 'c-other' },
    });
    expect(response.ok()).toBe(true);

    // The card names the session that raised it, which is the whole point: the
    // operator can approve it without navigating there first.
    const approval = page.getByTestId('approval');
    await expect(approval).toBeVisible();
    await expect(approval).toContainText('other');
    await expect(approval).toContainText('run_command');
    await expect(approval).toContainText('ls');

    await approval.getByRole('button', { name: 'Approve' }).click();
    await expect(approval).toContainText('waiting for the hub');

    await page.request.post(`${STUB}/__stub/settle`, {
        data: { session: 'other', confirmation_id: 'c-other', decision: 'approved' },
    });
    await expect(page.getByTestId('approval')).toHaveCount(0);
});

test('a message the operator sends appears and stays (D19)', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    await page.getByLabel('message').fill('please summarise the repository');
    await page.getByRole('button', { name: 'Send' }).click();

    const sent = page.getByTestId('outbox-item');
    await expect(sent).toContainText('please summarise the repository');
    // The worker admitted it, and the text is still there: `input_admitted`
    // carries no payload, so the panel is the only place it exists.
    await expect(sent).toHaveAttribute('data-state', 'admitted');
});

test('the page reports no console errors while loading and using a session', async ({ page }) => {
    const errors: string[] = [];
    page.on('console', (message) => {
        if (message.type() === 'error') errors.push(message.text());
    });
    page.on('pageerror', (error) => errors.push(error.message));

    await open(page);
    await emit(page, 'input_admitted', {}, { request_id: 'req-1' });
    await emit(page, 'model_response', answer('# hello\n\nwith a [link](https://example.com)'));
    await emit(page, 'run_finished', { status: 'completed' });
    await page.getByTestId('session-row').click();
    await expect(page.getByTestId('transcript')).toContainText('hello');
    // The switch changes what is rendered, so it is part of what must not
    // produce a console error.
    await page.getByTestId('details-toggle').check();
    await expect(page.getByTestId('protocol-line').first()).toBeVisible();

    expect(errors).toEqual([]);
});
