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
 * a real hub cannot be asked to do on cue.
 */
import { expect, test, type Page } from '@playwright/test';

const STUB = `http://127.0.0.1:${process.env.STUB_HUB_PORT ?? 4180}`;

/** Reset the stub, then load the panel. */
async function open(page: Page, query = ''): Promise<void> {
    await page.request.post(`${STUB}/__stub/reset`);
    await page.goto(`/app.html${query}`);
    await expect(page.getByText('connected', { exact: true })).toBeVisible();
}

/** Emit one envelope through the stub. */
async function emit(
    page: Page,
    event: string,
    data: unknown,
    extra: Record<string, unknown> = {},
): Promise<void> {
    const response = await page.request.post(`${STUB}/__stub/emit`, {
        data: { session: 'demo', event, data, extra },
    });
    expect(response.ok()).toBe(true);
}

/** A complete model response, as the worker protocol defines one. */
function modelResponse(text: string) {
    return {
        type: 'model_response',
        role: 'assistant',
        content: [{ type: 'text', raw: text }],
    };
}

test('the shell renders and lists the hub\'s sessions', async ({ page }) => {
    await open(page);

    await expect(page.getByRole('heading', { name: 'simplex hub' })).toBeVisible();
    await expect(page.getByTestId('session-row')).toHaveCount(1);
    await expect(page.getByText('No session selected')).toBeVisible();
});

test('selecting a session shows what the hub replays', async ({ page }) => {
    await open(page);
    await emit(page, 'model_response', modelResponse('replayed history'));
    await emit(page, 'model_response', modelResponse('and one more'));

    await page.getByTestId('session-row').click();

    await expect(page.getByTestId('transcript')).toContainText('replayed history');
    await expect(page.getByTestId('transcript')).toContainText('and one more');
});

test('a reconnecting panel keeps the transcript it already had (A2)', async ({ page }) => {
    await open(page);
    await emit(page, 'model_response', modelResponse('first answer'));
    await page.getByTestId('session-row').click();
    await expect(page.getByTestId('transcript')).toContainText('first answer');

    await emit(page, 'model_response', modelResponse('second answer'));
    await emit(page, 'model_response', modelResponse('third answer'));
    await expect(page.getByTestId('transcript')).toContainText('third answer');

    const before = await page.getByTestId('transcript-event').count();
    expect(before).toBeGreaterThanOrEqual(3);

    // The stub closes every panel socket and comes back with a new transcript
    // epoch and an empty transcript — which is both halves of A2 at once: the
    // reconnect itself, and the restart that makes the old cursor meaningless.
    await page.request.post(`${STUB}/__stub/restart`, { data: { epoch: 'stub-epoch-2' } });
    await expect(page.getByText('connected', { exact: true })).toBeVisible();
    await expect(page.getByTestId('transcript-note')).toContainText('hub restarted');

    // The new hub process has nothing to replay, so everything on screen is
    // what the panel kept. This is the assertion A2 is about.
    const after = await page.getByTestId('transcript-event').count();
    expect(after, 'the transcript was cleared by the reconnect').toBeGreaterThanOrEqual(before);
    await expect(page.getByTestId('transcript')).toContainText('first answer');
    await expect(page.getByTestId('transcript')).toContainText('second answer');
    await expect(page.getByTestId('transcript')).toContainText('third answer');

    // And the new process's numbering does not collide with the old one's: the
    // envelope it sends is numbered 1 again, and both are on screen.
    await emit(page, 'model_response', modelResponse('after the restart'));
    await expect(page.getByTestId('transcript')).toContainText('after the restart');
    await expect(page.getByTestId('transcript')).toContainText('first answer');
});

test('an approval for a session the panel is not watching still arrives (A1)', async ({ page }) => {
    await page.request.post(`${STUB}/__stub/reset`);
    await page.request.post(`${STUB}/__stub/sessions`, {
        data: {
            sessions: [
                {
                    session_id: 'watched', created_at: '2026-01-01T00:00:00.000Z', spec: {},
                    connected: true,
                    identity: { state: 'live', worker_id: 'w', since: null },
                    stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 1 },
                    last_run_id: '', last_event_at: null, last_event: null,
                    confirmations: [], process: null, requests: [],
                },
                {
                    session_id: 'other', created_at: '2026-01-02T00:00:00.000Z', spec: {},
                    connected: true,
                    identity: { state: 'live', worker_id: 'w', since: null },
                    stats: { events: 0, gaps: 0, duplicates: 0, protocolErrors: 0, incarnations: 1 },
                    last_run_id: '', last_event_at: null, last_event: null,
                    confirmations: [], process: null, requests: [],
                },
            ],
        },
    });
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
    await emit(page, 'model_response', modelResponse('hello'));
    await page.getByTestId('session-row').click();
    await expect(page.getByTestId('transcript')).toContainText('hello');

    expect(errors).toEqual([]);
});
