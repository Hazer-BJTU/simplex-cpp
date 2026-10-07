import { expect, test, type Page } from '@playwright/test';
import { STUB, call, emit, modelResponse, open, setSessions, toolResult } from './harness.ts';

type Counters = Record<string, { count: number; milliseconds: number }>;
async function counters(page: Page, reset = false): Promise<Counters> {
    return page.evaluate(reset => {
        const profile = (window as unknown as { __simplexPanelProfile: { read(): Counters; reset(): void } }).__simplexPanelProfile;
        const result = profile.read();
        if (reset) profile.reset();
        return result;
    }, reset);
}
async function approval(page: Page, id = 'first', session = 'demo') {
    await page.request.post(`${STUB}/__stub/confirm`, { data: { session, confirmation_id: id } });
    await expect(page.getByRole('dialog')).toBeVisible();
}

test('unchanged Markdown and completed rounds do not render for unrelated activity or approvals', async ({ page }) => {
    await open(page, '?session=demo&panel_profile=1');
    await emit(page, 'input_admitted', { operation: 'message' });
    await emit(page, 'model_response', modelResponse('## Answer\n\n```typescript\nconst value = 1;\n```'));
    await emit(page, 'run_finished', { status: 'completed' });
    await expect(page.getByTestId('round')).toContainText('Answer');
    await counters(page, true);
    await approval(page);
    await page.keyboard.press('Escape');
    await page.request.post(`${STUB}/__stub/message`, { data: { type: 'logs', session: 'demo', lines: ['unrelated log'], dropped: 0 } });
    await expect(page.getByRole('dialog')).toHaveCount(0);
    const report = await counters(page);
    expect(report.markdown?.count ?? 0).toBe(0);
    expect(report.roundBody?.count ?? 0).toBe(0);
    await page.getByTestId('approval-banner').getByRole('button', { name: 'Review' }).click();
    await expect(page.getByRole('dialog')).toBeVisible();
    await expect(page.getByRole('dialog').locator('details pre')).toBeEmpty();
    await page.getByRole('dialog').locator('details summary').click();
    await expect(page.getByRole('dialog').locator('details pre')).toContainText('ls');
});

test('hidden conversation skips visual derivation, preserves disclosures and shows current output on return', async ({ page }) => {
    await open(page, '?session=demo&panel_profile=1');
    await page.request.post(`${STUB}/__stub/plan`, { data: { plan: { markdown: '# Work', revision: 1, updated_at: null } } });
    await emit(page, 'input_admitted', { operation: 'message' });
    await emit(page, 'tool_calls', [call('work', 'run_command', { command: 'echo active' })]);
    const details = page.getByTestId('tool-details');
    await details.locator('summary').first().click();
    await expect(details).toHaveAttribute('open', '');
    await page.getByRole('tab', { name: 'Plan', exact: true }).click();
    await counters(page, true);
    for (let index = 0; index < 5; index++) await emit(page, 'model_response', modelResponse(`Hidden response ${index}`));
    await emit(page, 'tool_results', [toolResult('work', 'run_command', 'final output')]);
    const report = await counters(page);
    expect(report.rounds?.count ?? 0).toBe(0);
    expect(report.markdown?.count ?? 0).toBe(0);
    await page.getByRole('tab', { name: 'Conversation', exact: true }).click();
    await expect(page.getByTestId('transcript')).toContainText('Hidden response 4');
    await expect(details).toHaveAttribute('open', '');
    await expect(details).toContainText('final output');
    expect((await counters(page)).rounds?.count).toBe(1);
});

test('a running tool keeps the same mounted spinner through subsequent responses', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'input_admitted', { operation: 'message' });
    await emit(page, 'tool_calls', [call('work', 'run_command', { command: 'echo active' })]);
    const spinner = page.getByTestId('tool-status').locator('svg');
    await expect(spinner).toBeVisible();
    await spinner.evaluate(node => { (window as unknown as { __spinner: Element }).__spinner = node; });
    for (let index = 0; index < 5; index++) await emit(page, 'model_response', modelResponse(`Working ${index}`));
    expect(await spinner.evaluate(node => node === (window as unknown as { __spinner: Element }).__spinner)).toBe(true);
});

test('submission feedback keeps dialog geometry, locks one prompt and survives Later/Review', async ({ page }) => {
    await open(page, '?session=demo');
    await approval(page);
    const dialog = page.getByRole('dialog');
    await dialog.evaluate(async node => { await Promise.all(node.getAnimations().map(animation => animation.finished)); });
    const before = await dialog.boundingBox();
    await dialog.getByRole('button', { name: 'Approve', exact: true }).click();
    await expect(dialog.getByRole('button', { name: 'Approve', exact: true })).toBeDisabled();
    await expect(dialog.getByRole('button', { name: 'Deny', exact: true })).toBeDisabled();
    expect(await dialog.boundingBox()).toEqual(before);
    await expect(dialog).not.toContainText('sent — waiting');
    await dialog.getByRole('button', { name: 'Later', exact: true }).click();
    await expect(dialog).toHaveCount(0);
    await page.getByTestId('approval-banner').getByRole('button', { name: 'Review' }).click();
    await expect(dialog.getByRole('button', { name: 'Approve', exact: true })).toBeDisabled();
    expect((await (await page.request.get(`${STUB}/__stub/decisions`)).json()).decisions).toHaveLength(1);
    await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'first' } });
    await expect(dialog).toHaveCount(0);
});

test('a correlated rejection restores only its prompt while another session waits independently', async ({ page }) => {
    await open(page, '?session=demo');
    await setSessions(page, ['demo', 'other']);
    await page.request.post(`${STUB}/__stub/message`, { data: { type: 'sessions',
        sessions: (await (await page.request.get(`${STUB}/api/sessions`)).json()).sessions } });
    await approval(page, 'first');
    await page.getByRole('dialog').getByRole('button', { name: 'Approve', exact: true }).click();
    await page.keyboard.press('Escape');
    await approval(page, 'second', 'other');
    await page.getByRole('dialog').getByRole('button', { name: 'Deny', exact: true }).click();
    const decisions = (await (await page.request.get(`${STUB}/__stub/decisions`)).json()).decisions;
    await page.request.post(`${STUB}/__stub/message`, { data: {
        type: 'error', error: 'confirmation_rejected', message: 'first refused', request: decisions[0],
    } });
    await page.keyboard.press('Escape');
    // Select scoped rows directly: sibling prompts cannot share in-flight state.
    await expect(page.locator('[data-confirmation="first"]').getByRole('button', { name: 'Approve', exact: true })).toBeEnabled();
    await expect(page.locator('[data-confirmation="second"]').getByRole('button', { name: 'Deny', exact: true })).toBeDisabled();
});

test('reader intent keeps an older anchor while growth arrives; jump restores following', async ({ page }) => {
    await open(page, '?session=demo');
    await emit(page, 'input_admitted', { operation: 'message' });
    await emit(page, 'model_response', modelResponse('Paragraph for reading.\n\n'.repeat(100)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await scroller.hover();
    await page.mouse.wheel(0, -600);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    const position = await scroller.evaluate(node => node.scrollTop);
    await emit(page, 'model_response', modelResponse('New output\n\n'.repeat(20)));
    await expect(page.getByTestId('transcript')).toContainText('New output');
    await expect.poll(() => scroller.evaluate(node => node.scrollTop)).toBeGreaterThan(position - 2);
    expect(await scroller.evaluate(node => node.scrollTop)).toBeLessThan(position + 2);
    await page.getByRole('button', { name: 'jump to latest' }).click();
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
});

test('burst publication preserves all responses and urgent terminal state without animation frames', async ({ page }) => {
    await open(page, '?session=demo&panel_profile=1');
    await emit(page, 'input_admitted', { operation: 'message' });
    await emit(page, 'run_started', {});
    // A hidden/throttled tab may stop rAF. The store still ingests immediately,
    // and the bounded timer must publish ordinary output without relying on it.
    await page.evaluate(() => {
        const target = window as unknown as { __raf: typeof requestAnimationFrame };
        target.__raf = requestAnimationFrame;
        window.requestAnimationFrame = () => 0;
    });
    await page.request.post(`${STUB}/__stub/emit-batch`, { data: { events: Array.from({ length: 40 }, (_, index) => ({
        event: 'model_response', data: modelResponse(`Ordered response ${index}`),
    })) } });
    await expect(page.getByTestId('assistant-message')).toHaveCount(40);
    expect((await page.getByTestId('assistant-message').allTextContents()).map(text => text.slice(text.indexOf('Ordered response')))).toEqual(
        Array.from({ length: 40 }, (_, index) => `Ordered response ${index}`));
    await emit(page, 'run_finished', { status: 'completed' });
    await expect(page.getByTestId('run-activity')).toHaveCount(0);
    await expect(page.getByRole('button', { name: 'Send', exact: true })).toBeVisible();
    expect((await counters(page)).eventFold?.count).toBe(43);
    await page.evaluate(() => { window.requestAnimationFrame = (window as unknown as { __raf: typeof requestAnimationFrame }).__raf; });
});

test('worker disconnect recovers an in-flight approval while the panel socket stays connected', async ({ page }) => {
    await open(page, '?session=demo');
    await approval(page);
    const dialog = page.getByRole('dialog');
    await dialog.getByRole('button', { name: 'Approve', exact: true }).click();
    await page.request.post(`${STUB}/__stub/connection`, { data: { connected: false } });
    await expect(dialog).toContainText('decision outcome is unknown');
    await expect(dialog.getByRole('button', { name: 'Deny', exact: true })).toBeDisabled();
    await page.request.post(`${STUB}/__stub/connection`, { data: { connected: true } });
    await expect(dialog.getByRole('button', { name: 'Approve', exact: true })).toBeEnabled();
    await expect(dialog).toContainText('still lists this prompt as open');
    expect((await (await page.request.get(`${STUB}/__stub/decisions`)).json()).decisions).toHaveLength(1);
});

test('an unknown approval outcome can be checked in the open dialog without sending another decision', async ({ page }) => {
    await open(page, '?session=demo');
    await approval(page);
    const dialog = page.getByRole('dialog');
    await dialog.getByRole('button', { name: 'Approve', exact: true }).click();
    await page.route('**/api/sessions/demo', route => route.abort());
    // A legacy rejection lacks attempt correlation, so check the authoritative
    // snapshot instead of treating it as permission to submit another decision.
    await page.request.post(`${STUB}/__stub/message`, { data: {
        type: 'error', error: 'confirmation_rejected', message: 'unknown outcome',
        request: { type: 'confirmation', session: 'demo', confirmation_id: 'first' },
    } });
    await expect(dialog).toContainText('Could not check the decision outcome');
    await expect(dialog.getByRole('button', { name: 'Deny', exact: true })).toBeDisabled();
    await expect(dialog.getByRole('button', { name: 'Check outcome', exact: true })).toBeEnabled();
    await page.unroute('**/api/sessions/demo');
    await dialog.getByRole('button', { name: 'Check outcome', exact: true }).click();
    await expect(dialog).toContainText('still lists this prompt as open');
    await expect(dialog.getByRole('button', { name: 'Approve', exact: true })).toBeEnabled();
    expect((await (await page.request.get(`${STUB}/__stub/decisions`)).json()).decisions).toHaveLength(1);
});

test('a reader retains their anchor and offset across Plan and approval-region resizing', async ({ page }) => {
    await open(page, '?session=demo');
    await page.request.post(`${STUB}/__stub/plan`, { data: { plan: { markdown: '# Work', revision: 1, updated_at: null } } });
    await emit(page, 'input_admitted', { operation: 'message' });
    await emit(page, 'model_response', modelResponse('Earlier paragraph.\n\n'.repeat(120)));
    const scroller = page.getByTestId('transcript');
    await expect.poll(() => scroller.evaluate(node => node.scrollHeight - node.scrollTop - node.clientHeight)).toBeLessThan(2);
    await scroller.hover();
    await page.mouse.wheel(0, -600);
    await expect(page.getByRole('button', { name: 'jump to latest' })).toBeVisible();
    const before = await scroller.evaluate(node => node.scrollTop);
    await page.getByRole('tab', { name: 'Plan', exact: true }).click();
    await emit(page, 'model_response', modelResponse('Arrived in Plan'));
    await page.getByRole('tab', { name: 'Conversation', exact: true }).click();
    await expect(scroller).toContainText('Arrived in Plan');
    expect(Math.abs(await scroller.evaluate(node => node.scrollTop) - before)).toBeLessThanOrEqual(2);
    await approval(page);
    await page.keyboard.press('Escape');
    await expect(page.getByRole('dialog')).toHaveCount(0);
    expect(Math.abs(await scroller.evaluate(node => node.scrollTop) - before)).toBeLessThanOrEqual(2);
    await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'first' } });
    await expect(page.getByTestId('approvals')).toHaveCount(0);
    expect(Math.abs(await scroller.evaluate(node => node.scrollTop) - before)).toBeLessThanOrEqual(2);
});
