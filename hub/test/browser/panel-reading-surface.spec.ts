/** Comparable review artifacts plus interaction checks for the reading surface. */
import { expect, test } from '@playwright/test';
import { STUB, call, emit, modelResponse, open, toolResult } from './harness.ts';

test.use({ video: 'on' });

const prose = `## A small, complete result

The worker keeps the task and its context together. Tools remain available when you need to inspect what happened, while the conversation stays easy to read.

| Component | Responsibility |
| --- | --- |
| Worker | Execute the request and preserve state |
| Hub | Connect sessions and route approvals |
| Panel | Present the conversation and its controls |

\`\`\`cpp
// A complete response appears once the model request finishes.
auto result = co_await run(state, model, registry, options);
\`\`\`

The next step is to review the result, then continue only if more work is needed.`;

for (const theme of ['light', 'dark'] as const) {
    for (const width of [360, 768, 1440]) {
        test(`reading surface preview ${theme} ${width}`, async ({ page }, testInfo) => {
            await page.setViewportSize({ width, height: 960 });
            await page.addInitScript((value) => localStorage.setItem('simplex.panel.theme', value), theme);
            await open(page, '?session=demo');
            await expect(page.getByLabel('message', { exact: true })).toBeVisible();
            await page.getByLabel('message', { exact: true }).fill('Explain the result and show the implementation.');
            await page.getByRole('button', { name: 'Send', exact: true }).click();
            await emit(page, 'run_started', {});
            await emit(page, 'model_response', modelResponse(prose));
            await emit(page, 'tool_calls', [call('read', 'read_text', { path: '/workspace/notes.md' })]);
            await emit(page, 'tool_results', [toolResult('read', 'read_text', 'Implementation checked.')]);
            await emit(page, 'run_finished', { status: 'completed', exchanges: 1 });
            await expect(page.getByTestId('assistant-message')).toContainText('A small, complete result');
            await page.screenshot({ animations: 'disabled', path: testInfo.outputPath('conversation.png') });
            await testInfo.attach('conversation', { path: testInfo.outputPath('conversation.png'), contentType: 'image/png' });

            await page.getByLabel('message', { exact: true }).fill('Continue the work.');
            await page.getByRole('button', { name: 'Send', exact: true }).click();
            await emit(page, 'run_started', {});
            await emit(page, 'tool_calls', [call('auto', 'auto_compact', { reason: 'token_threshold' }, {
                security: 'trusted', type: 'serial_write', extras: { origin: 'worker', operation: 'auto_compact' },
            })]);
            await page.getByRole('button', { name: 'Cancel run', exact: true }).click();
            await expect(page.getByRole('button', { name: 'Cancelling…', exact: true })).toBeDisabled();
            await page.screenshot({ animations: 'disabled', path: testInfo.outputPath('cancellation.png') });
            await testInfo.attach('cancellation', { path: testInfo.outputPath('cancellation.png'), contentType: 'image/png' });

            await emit(page, 'run_finished', { status: 'cancelled' });
            await page.getByLabel('message', { exact: true }).fill('Resume the analysis.');
            await page.getByRole('button', { name: 'Send', exact: true }).click();
            await emit(page, 'run_started', {});
            await emit(page, 'run_finished', { status: 'failed', failure: {
                stage: 'model_request', can_continue: true,
            }, error: 'The model service could not complete this request.' });
            await expect(page.getByTestId('run-failure')).toContainText('Model request failed');
            await page.screenshot({ animations: 'disabled', path: testInfo.outputPath('failure.png') });
            await testInfo.attach('failure', { path: testInfo.outputPath('failure.png'), contentType: 'image/png' });
            await page.getByLabel('message', { exact: true }).press('Alt+Enter');
            await page.getByLabel('command input').fill('continue');
            await page.getByLabel('command input').press('Enter');
            await emit(page, 'run_started', {});
            await emit(page, 'model_response', modelResponse('The analysis resumed from the saved state.'));
            await emit(page, 'run_finished', { status: 'completed' });
            const continued = page.getByTestId('round').last();
            await expect(continued).toContainText('The analysis resumed from the saved state.');
            await expect(continued.getByTestId('outbox-item')).toHaveCount(0);
            await expect(continued.getByTestId('admitted-placeholder')).toHaveCount(0);
            // A new panel can restore the simplified worker history even when
            // the Hub no longer has detailed execution events to replay.
            await page.request.post(`${STUB}/__stub/reset`);
            await page.request.post(`${STUB}/__stub/settings`, { data: {
                historyEnabled: true,
                historyTurns: [{ index: 0,
                    user: [{ type: 'text', raw: 'Resume the analysis.', modality: 'text' }],
                    steps: [{ index: 0, tool_calls: 0, content: [{ type: 'text',
                        raw: 'The analysis resumed from the saved state.', modality: 'text' }] }],
                    omitted_steps: 0,
                }],
            } });
            await emit(page, 'ready', { active: false, capabilities: ['session-history'] });
            await page.reload();
            await expect(page.getByTestId('history-turn')).toContainText('The analysis resumed from the saved state.');
            await page.screenshot({ animations: 'disabled', path: testInfo.outputPath('restored.png') });
            await testInfo.attach('restored', { path: testInfo.outputPath('restored.png'), contentType: 'image/png' });

            await page.request.post(`${STUB}/__stub/confirm`, { data: {
                session: 'demo', confirmation_id: 'long-approval',
                call: call('edit', 'str_replace_edit', { path: '/workspace/notes.md',
                    old_str: 'Original line\n'.repeat(40), new_str: 'Replacement line\n'.repeat(40) }, { security: 'require_confirm', type: 'serial_write' }),
            } });
            await expect(page.getByRole('dialog')).toBeVisible();
            await page.getByRole('dialog').getByText('arguments as the worker sent them').click();
            await page.screenshot({ animations: 'disabled', path: testInfo.outputPath('approval.png') });
            await testInfo.attach('approval', { path: testInfo.outputPath('approval.png'), contentType: 'image/png' });
        });
    }
}

// These assertions are separate from review captures: visual acceptance does
// not replace interaction guarantees, and an image is not a regression test.
test('reading edges align, mode switches preserve selection, and updates do not move controls', async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 960 });
    await open(page, '?session=demo');
    const input = page.getByLabel('message', { exact: true });
    await input.fill('A draft to keep.\n'.repeat(30));
    const surface = page.getByTestId('composer-surface');
    const before = (await surface.boundingBox())!;
    const reading = (await page.getByTestId('reading-surface').boundingBox())!;
    expect(reading.x).toBe(before.x);
    expect(reading.width).toBe(before.width);
    expect(reading.width).toBe(832);
    await input.evaluate((node: HTMLTextAreaElement) => node.setSelectionRange(2, 7));
    await input.press('Alt+Enter');
    const command = page.getByLabel('command input');
    await expect(command).toBeFocused();
    expect(await surface.boundingBox()).toEqual(before);
    await command.fill('ref');
    expect(await surface.boundingBox()).toEqual(before);
    await command.press('Alt+Enter');
    expect(await input.evaluate((node: HTMLTextAreaElement) => [node.selectionStart, node.selectionEnd])).toEqual([2, 7]);
    expect(await surface.boundingBox()).toEqual(before);
    await input.dispatchEvent('keydown', { key: 'Enter', altKey: true, isComposing: true, bubbles: true });
    await expect(input).toBeVisible();
    await emit(page, 'run_started', {});
    const cancel = page.getByRole('button', { name: 'Cancel run', exact: true });
    const action = await cancel.boundingBox();
    expect(await surface.boundingBox()).toEqual(before);
    await emit(page, 'model_response', modelResponse('Working', { cost: { prompt: 128000, generated: 200, cache_hit: 64000 } }));
    expect(await surface.boundingBox()).toEqual(before);
    await cancel.click();
    const pending = page.getByRole('button', { name: 'Cancelling…', exact: true });
    await expect(pending).toBeDisabled();
    expect(await pending.boundingBox()).toEqual(action);
    expect(await surface.boundingBox()).toEqual(before);
});

test('expired and externally settled approvals cannot retain actionable buttons', async ({ page }) => {
    await open(page, '?session=demo');
    await page.request.post(`${STUB}/__stub/confirm`, { data: {
        confirmation_id: 'expires', deadline_at: new Date(Date.now() + 1200).toISOString(),
    } });
    const dialog = page.getByRole('dialog');
    await expect(dialog).toBeVisible();
    await expect(dialog.getByRole('button', { name: 'Approve', exact: true })).toBeEnabled();
    await expect(dialog.getByRole('button', { name: 'Approve', exact: true })).toBeDisabled();
    await expect(dialog.getByRole('button', { name: 'Deny', exact: true })).toBeDisabled();
    await expect(dialog).toContainText('Approval deadline passed');
    await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'expires', decision: 'denied' } });
    await expect(dialog).toHaveCount(0);
    await page.request.post(`${STUB}/__stub/confirm`, { data: { confirmation_id: 'elsewhere' } });
    await expect(dialog).toBeVisible();
    await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'elsewhere', decision: 'approved' } });
    await expect(dialog).toHaveCount(0);
    await expect(page.getByTestId('approval-banner')).toHaveCount(0);
});

test('narrow and short viewports bound the composer, suggestions, and long output', async ({ page }) => {
    await page.setViewportSize({ width: 360, height: 480 });
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await open(page, '?session=demo');
    await emit(page, 'model_response', modelResponse('```text\n' + 'long_column_'.repeat(200) + '\n```'));
    const input = page.getByLabel('message', { exact: true });
    await input.fill('Long draft\n'.repeat(80));
    await input.press('Alt+Enter');
    const suggestions = page.getByRole('listbox', { name: 'matching commands' });
    const bounds = (await suggestions.boundingBox())!;
    expect(bounds.y).toBeGreaterThanOrEqual(0);
    expect(bounds.x + bounds.width).toBeLessThanOrEqual(360);
    const send = (await page.getByRole('button', { name: 'Send', exact: true }).boundingBox())!;
    expect(send.y + send.height).toBeLessThanOrEqual(480);
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(360);
});

test('tool expansion survives new results and zoom keeps controls reachable', async ({ page }) => {
    await page.setViewportSize({ width: 1440, height: 960 });
    await open(page, '?session=demo');
    await emit(page, 'run_started', {});
    await emit(page, 'tool_calls', [call('one', 'read_text', { path: '/workspace/notes.md' })]);
    const details = page.getByTestId('tool-details');
    await details.locator('summary').click();
    await expect(details).toHaveAttribute('open', '');
    await emit(page, 'tool_results', [toolResult('one', 'read_text', 'A full result')]);
    await emit(page, 'model_response', modelResponse('Finished inspecting the file.'));
    await expect(details).toHaveAttribute('open', '');
    await expect(details).toContainText('A full result');
    await page.evaluate(() => { document.documentElement.style.zoom = '2'; });
    await expect(page.getByRole('button', { name: 'Cancel run', exact: true })).toBeInViewport();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth)).toBe(true);
});

test('Inspector retains diagnostic identity without a worker status snapshot', async ({ page }) => {
    await open(page, '?session=demo');
    await expect(page.getByTestId('details-toggle')).toHaveCount(0);
    await page.getByRole('button', { name: 'Show inspector', exact: true }).click();
    const inspector = page.getByTestId('inspector');
    await expect(inspector).toContainText('No status snapshot yet');
    await expect(inspector).toContainText('panel protocol');
    await expect(inspector).toContainText('stub-epoch');
    await expect(inspector.getByRole('button', { name: 'raw status', exact: true })).toBeDisabled();
});

test('classic scrollbar space does not offset the shared reading edges', async ({ page }) => {
    await page.setViewportSize({ width: 768, height: 960 });
    await open(page, '?session=demo');
    await page.addStyleTag({ content: '.reading-scroll { overflow-y: scroll !important; } .reading-scroll::-webkit-scrollbar { width: 16px; }' });
    await emit(page, 'model_response', modelResponse('A long response.\n\n'.repeat(100)));
    await expect.poll(async () => {
        const reading = (await page.getByTestId('reading-surface').boundingBox())!;
        const composer = (await page.getByTestId('composer-surface').boundingBox())!;
        return Math.abs(reading.x - composer.x) + Math.abs(reading.width - composer.width);
    }).toBeLessThan(1);
});
