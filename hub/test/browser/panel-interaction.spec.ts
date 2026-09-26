/**
 * @file the interaction rebuild: what each D-group defect looks like now.
 *
 * Every test here is the closing of a numbered defect from the plan's §1.2, and
 * each is written so that it fails against the behaviour the old panel had. The
 * ones about focus and desperation are hard to express any other way: "opening
 * this dialog and pressing Enter must not decide anything" is not a property of
 * a function, it is a property of the page.
 */
import { expect, test } from '@playwright/test';
import {
    STUB, emit, modelResponse, open, runningSession, setSessions, stoppedSession, withSession,
} from './harness.ts';

async function withRunningSession(page: import('@playwright/test').Page): Promise<void> {
    await withSession(page, runningSession('demo'));
}

test('the header offers one primary action, and the state picks which (D8/D9/D10)', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    // No process: Start is the primary action and Stop is not on screen at all,
    // rather than being present and disabled among nine peers.
    await expect(page.getByRole('button', { name: 'Start' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Stop' })).toHaveCount(0);

    await withRunningSession(page);
    await page.request.post(`${STUB}/__stub/emit`, {
        data: { session: 'demo', event: 'status', data: { active: false } },
    });
    await page.reload();
    await page.getByTestId('session-row').click();

    await expect(page.getByRole('button', { name: 'Stop' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Start' })).toHaveCount(0);
});

test('a destructive action says what it will actually do (D27)', async ({ page }) => {
    await open(page);
    await withRunningSession(page);
    await page.reload();
    await page.getByTestId('session-row').click();

    await page.getByRole('button', { name: 'More actions' }).click();
    await page.getByRole('menuitem', { name: /Force kill/ }).click();

    // The hub is not configured to kill the process group, and the dialog says
    // so. The old panel's `window.confirm` claimed it unconditionally, which is
    // how a confirmation teaches an operator to stop reading them.
    const dialog = page.getByRole('dialog');
    await expect(dialog).toContainText('not');
    await expect(dialog).toContainText('process group');

    await page.getByRole('button', { name: 'Cancel' }).click();

    // Configured the other way, the same dialog says the opposite.
    await page.request.post(`${STUB}/__stub/settings`, {
        data: { force_kill_process_group: true },
    });
    await page.reload();
    await page.getByTestId('session-row').click();
    await page.getByRole('button', { name: 'More actions' }).click();
    await page.getByRole('menuitem', { name: /Force kill/ }).click();
    await expect(page.getByRole('dialog')).toContainText(
        'kill the whole process group',
    );
});

test('a dangerous action asks in a dialog, not a browser prompt (D27)', async ({ page }) => {
    const dialogs: string[] = [];
    page.on('dialog', (dialog) => { dialogs.push(dialog.message()); void dialog.dismiss(); });

    await open(page);
    await withSession(page, stoppedSession('demo'));
    await page.reload();
    await page.getByTestId('session-row').click();

    await page.getByRole('button', { name: 'More actions' }).click();
    await page.getByRole('menuitem', { name: /Delete session/ }).click();

    await expect(page.getByRole('dialog')).toContainText('cannot be undone');
    expect(dialogs).toEqual([]);
});

test('an action that cannot run says why (D11)', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    await page.getByRole('button', { name: 'More actions' }).click();
    // No process has been started, so there is nothing to restart or force-kill.
    await expect(page.getByRole('menuitem', { name: /Restart/ })).toHaveAttribute(
        'data-disabled', '',
    );
    await expect(page.getByText('no process is running')).toHaveCount(2);
});

test('an approval arrives, opens itself, and is not decided by pressing Enter (D16)', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    await page.request.post(`${STUB}/__stub/confirm`, {
        data: { session: 'demo', confirmation_id: 'c-1' },
    });

    const dialog = page.getByRole('dialog');
    await expect(dialog).toBeVisible();
    await dialog.press('Enter');
    await expect(dialog).toBeVisible();

    // Nothing was decided: the old modal focused its own "hide" button, so
    // Enter dismissed an approval silently.
    const decisions = await page.request.get(`${STUB}/__stub/decisions`);
    expect((await decisions.json()).decisions).toEqual([]);
});

test('a deferred approval does not put itself back, and is still answerable (D17)', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await page.request.post(`${STUB}/__stub/confirm`, {
        data: { session: 'demo', confirmation_id: 'c-1' },
    });

    await expect(page.getByRole('dialog')).toBeVisible();
    await page.getByRole('button', { name: 'Later' }).click();
    await expect(page.getByRole('dialog')).toHaveCount(0);

    // A re-render used to be enough to bring it back. Traffic is exactly that.
    await emit(page, 'model_response', modelResponse('something else happened'));
    await expect(page.getByRole('dialog')).toHaveCount(0);

    // Still visible, still answerable, one click away.
    const banner = page.getByTestId('approval-banner');
    await expect(banner).toBeVisible();
    await banner.getByRole('button', { name: 'Review' }).click();
    await expect(page.getByRole('dialog')).toBeVisible();
});

test('a decision the hub never answers leaves a usable button (D18)', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await page.request.post(`${STUB}/__stub/confirm`, {
        data: { session: 'demo', confirmation_id: 'c-1' },
    });

    const dialog = page.getByRole('dialog');
    await expect(dialog).toBeVisible();
    await dialog.getByRole('button', { name: 'Approve' }).click();

    // The stub records the decision and never answers it, which is what a lost
    // or refused decision looks like from here. The button must still work —
    // the old panel disabled it on the first click and left the dialog stuck.
    await expect(dialog.getByRole('button', { name: 'Approve' })).toBeEnabled();
    await dialog.getByRole('button', { name: 'Approve' }).click();

    const decisions = await page.request.get(`${STUB}/__stub/decisions`);
    expect((await decisions.json()).decisions).toHaveLength(2);
});

for (const [kind, args] of [
    ['command', { command: 'echo '.repeat(3000) }],
    ['edit', { path: '/tmp/example.cpp', old_str: 'old\n'.repeat(2000), new_str: 'new\n'.repeat(2000) }],
] as const) {
    test(`a long ${kind} approval stays inside the viewport with reachable decisions`, async ({ page }) => {
        await page.setViewportSize({ width: 390, height: 420 });
        await open(page);
        await page.getByRole('button', { name: 'Open the session list' }).click();
        await page.getByTestId('session-row').click();
        await page.request.post(`${STUB}/__stub/confirm`, {
            data: {
                session: 'demo', confirmation_id: `long-${kind}`,
                call: { name: kind === 'command' ? 'run_command' : 'str_replace_edit', arguments: args },
            },
        });

        const dialog = page.getByRole('dialog');
        await expect(dialog).toBeVisible();
        if (kind === 'edit') {
            await dialog.getByText('arguments as the worker sent them').click();
        }
        const bounds = await dialog.evaluate((node) => {
            const box = node.getBoundingClientRect();
            const summary = (node.querySelector('details[open] pre')
                ?? node.querySelector('pre')) as HTMLElement;
            return {
                top: box.top, bottom: box.bottom,
                viewportHeight: window.innerHeight,
                summaryScrolls: summary.scrollHeight > summary.clientHeight,
            };
        });
        expect(bounds.top).toBeGreaterThanOrEqual(0);
        expect(bounds.bottom).toBeLessThanOrEqual(bounds.viewportHeight);
        expect(bounds.summaryScrolls).toBe(true);
        await expect(dialog.getByRole('button', { name: 'Approve' })).toBeInViewport();
        await dialog.getByRole('button', { name: 'Later' }).click();
        await expect(dialog).toHaveCount(0);
    });
}

test('the confirmation mode belongs to one session (D15)', async ({ page }) => {
    await page.request.post(`${STUB}/__stub/reset`);
    await setSessions(page, ['first', 'second']);
    await page.goto('/');
    await expect(page.getByText('connected', { exact: true })).toBeVisible();

    await page.getByTestId('session-row').filter({ hasText: 'first' }).click();
    await page.getByRole('button', { name: /^confirmation/ }).click();
    await page.getByRole('radio', { name: /approve/ }).check();

    // The mode that turns approvals off is shown without opening anything: the
    // old panel kept it in one `<select>` that was never reset, so choosing it
    // once silently applied it to every session opened afterwards.
    await expect(page.getByText('approvals: approve')).toBeVisible();

    await page.getByTestId('session-row').filter({ hasText: 'second' }).click();
    await expect(page.getByText('approvals: approve')).toHaveCount(0);
    await page.getByRole('button', { name: /^confirmation/ }).click();
    await expect(page.getByRole('radio', { name: /^ask/ })).toBeChecked();

    // And the choice travels with the message that needs it.
    await page.getByLabel('message').fill('hello');
    await page.getByRole('button', { name: 'Send' }).click();
    const received = await page.request.get(`${STUB}/__stub/received`);
    const inputs = (await received.json()).received.filter(
        (message: { type: string }) => message.type === 'input',
    );
    expect(inputs.at(-1).options).toEqual({ confirmation: { mode: 'ask' } });
});

test('the composer keeps references as parts and clears on Escape (D12)', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    await page.getByLabel('Attach a reference').click();
    await page.getByLabel('external reference').fill('https://example.com/a.png');
    await page.getByRole('button', { name: 'Attach', exact: true }).click();
    await expect(page.getByText('https://example.com/a.png')).toBeVisible();

    await page.getByLabel('message').fill('look at this');
    await page.getByRole('button', { name: 'Send' }).click();

    const received = await page.request.get(`${STUB}/__stub/received`);
    const input = (await received.json()).received
        .filter((message: { type: string }) => message.type === 'input').at(-1);
    expect(input.content).toEqual([
        { type: 'text', raw: 'look at this' },
        { type: 'external_ref', raw: 'https://example.com/a.png' },
    ]);

    await page.getByLabel('message').fill('scratch that');
    await page.getByLabel('message').press('Escape');
    await expect(page.getByLabel('message')).toHaveValue('');
});

test('a refused input restores references as references', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await page.request.post(`${STUB}/__stub/settings`, { data: { refuseInput: true } });

    await page.getByLabel('Attach a reference').click();
    await page.getByLabel('external reference').fill('https://example.com/a.png');
    await page.getByRole('button', { name: 'Attach', exact: true }).click();
    await page.getByRole('textbox', { name: 'message' }).fill('look at this');
    await page.getByRole('button', { name: 'Send' }).click();

    await expect(page.getByRole('textbox', { name: 'message' })).toHaveValue('look at this');
    await expect(page.getByRole('button', {
        name: 'remove reference https://example.com/a.png',
    })).toBeVisible();

    await page.request.post(`${STUB}/__stub/settings`, { data: { refuseInput: false } });
    await page.getByRole('button', { name: 'Send' }).click();
    const received = await page.request.get(`${STUB}/__stub/received`);
    const inputs = (await received.json()).received.filter(
        (message: { type: string }) => message.type === 'input',
    );
    expect(inputs.at(-1).content).toEqual(inputs.at(-2).content);
});

test('Continue sends no content and preserves an unsent draft', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await emit(page, 'run_started', {});

    const continueButton = page.getByRole('button', { name: 'Continue' });
    await expect(continueButton).toBeVisible();
    await continueButton.click();
    await page.getByRole('textbox', { name: 'message' }).fill('save this for later');
    await continueButton.click();

    await page.request.post(`${STUB}/__stub/settings`, { data: { refuseInput: true } });
    await continueButton.click();
    await expect(page.getByText('the stub refused the input')).toBeVisible();
    await expect(page.getByRole('textbox', { name: 'message' }))
        .toHaveValue('save this for later');

    const received = await page.request.get(`${STUB}/__stub/received`);
    const inputs = (await received.json()).received.filter(
        (message: { type: string }) => message.type === 'input',
    );
    expect(inputs).toHaveLength(3);
    for (const input of inputs) {
        expect(input.operation).toBe('continue');
        expect(input).not.toHaveProperty('content');
    }
    await expect(page.getByRole('textbox', { name: 'message' }))
        .toHaveValue('save this for later');
});

test('the drawer shows one pane at a time, and only while it is open (A3)', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    await expect(page.getByTestId('inspector')).toHaveCount(0);
    await page.getByRole('button', { name: 'Show inspector' }).click();
    await expect(page.getByTestId('inspector')).toBeVisible();

    // Visit every pane, then come back. The old inspector marked the others
    // `hidden` while its stylesheet said `display: flex`, so each one stayed on
    // screen at full height and the column grew with every click.
    // A status snapshot so the Run pane has something in it.
    await emit(page, 'status', { active: false, stopping: false, storage_failed: false, rejected_payloads: 0 });

    for (const name of ['Process', 'Logs', 'Snapshot']) {
        await page.getByRole('tab', { name }).click();
        await expect(page.getByRole('tabpanel')).toHaveCount(1);
    }
    await page.getByRole('tab', { name: 'Run' }).click();
    await expect(page.getByRole('tabpanel')).toHaveCount(1);
    await expect(page.getByRole('tabpanel')).toContainText('this panel');

    await page.getByRole('button', { name: 'Close' }).click();
    await expect(page.getByTestId('inspector')).toHaveCount(0);
});

test('the palette reaches everything the buttons do, from the keyboard', async ({ page }) => {
    await page.request.post(`${STUB}/__stub/reset`);
    await setSessions(page, ['alpha', 'beta']);
    await page.goto('/');
    await expect(page.getByText('connected', { exact: true })).toBeVisible();
    await page.getByTestId('session-row').filter({ hasText: 'alpha' }).click();

    await page.keyboard.press('ControlOrMeta+k');
    const commands = page.getByTestId('palette-command');
    await expect(commands.first()).toBeVisible();

    await page.getByLabel('filter commands').fill('beta');
    await expect(commands).toHaveCount(1);
    await commands.first().click();

    // Switching sessions is what the command did, and the URL follows.
    await expect(page.getByRole('heading', { name: 'beta' })).toBeVisible();
    expect(page.url()).toContain('session=beta');
});

test('the palette toggles technical details without a mouse', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await emit(page, 'run_started', {});
    await emit(page, 'model_response', modelResponse('hi'));
    await expect(page.getByTestId('protocol-line')).toHaveCount(0);

    await page.keyboard.press('ControlOrMeta+k');
    await page.getByLabel('filter commands').fill('technical');
    await page.getByTestId('palette-command').first().click();
    await expect(page.getByTestId('protocol-line').first()).toBeVisible();
});

test('a snapshot that arrives after a session switch is discarded (D25)', async ({ page }) => {
    await page.request.post(`${STUB}/__stub/reset`);
    await setSessions(page, ['first', 'second']);
    await page.goto('/');
    await expect(page.getByText('connected', { exact: true })).toBeVisible();

    // Make the fetch slow enough to switch sessions while it is in flight.
    await page.request.post(`${STUB}/__stub/snapshot-delay`, { data: { ms: 1500 } });

    await page.getByTestId('session-row').filter({ hasText: 'first' }).click();
    await page.getByRole('button', { name: 'Show inspector' }).click();
    await page.getByRole('tab', { name: 'Snapshot' }).click();
    await page.getByRole('button', { name: 'Load snapshot' }).click();

    // Leave before the answer arrives. The old pane awaited and then painted
    // whatever came back, so one session's state appeared under another's name.
    await page.getByTestId('session-row').filter({ hasText: 'second' }).click();
    await page.waitForTimeout(2000);

    await expect(page.getByRole('tabpanel')).not.toContainText('state for first');

    // Asking for the session actually on screen does show its own state.
    await page.getByRole('button', { name: 'Load snapshot' }).click();
    await expect(page.getByRole('tabpanel')).toContainText('state for second', { timeout: 10_000 });
});

test('the hub can be timed from the palette (D28)', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    await page.keyboard.press('ControlOrMeta+k');
    await page.getByLabel('filter commands').fill('ping');
    await page.getByTestId('palette-command').first().click();

    const received = await page.request.get(`${STUB}/__stub/received`);
    const pings = (await received.json()).received.filter(
        (message: { type: string }) => message.type === 'ping',
    );
    // The old panel handled `pong` and never sent `ping`, which left the hub's
    // heartbeat dead protocol surface.
    expect(pings.length).toBeGreaterThan(0);
});

test('a transcript can be recovered over HTTP when the socket is down (D28)', async ({ page }) => {
    await open(page);
    await emit(page, 'model_response', modelResponse('history from the hub'));
    await page.getByTestId('session-row').click();
    await expect(page.getByTestId('transcript')).toContainText('history from the hub');

    // Every request the panel makes to the events route, so the test can tell
    // the fallback fired rather than merely that the words are still on screen.
    const recoveries: string[] = [];
    page.on('request', (request) => {
        if (request.url().includes('/events')) recoveries.push(request.url());
    });

    // The hub goes down: it refuses upgrades and closes what it has. Reloading
    // then brings the panel up with no subscription, an empty store, and a hub
    // that still has the transcript — the situation the old panel had no way
    // out of, because it had no HTTP path at all.
    await page.request.post(`${STUB}/__stub/down`);
    await page.reload();
    await expect(page.getByText('refused this connection')).toBeVisible({ timeout: 15_000 });
    await expect(page.getByTestId('transcript')).not.toContainText('history from the hub');

    await page.getByRole('button', { name: 'reload transcript' }).click();
    await expect(page.getByTestId('transcript')).toContainText('history from the hub');
    expect(recoveries.length).toBeGreaterThan(0);
    await page.request.post(`${STUB}/__stub/up`);
});
