/**
 * @file the transcript: markdown, code, tools, folding, and what is not HTML.
 *
 * These are the claims that need a real browser. Markdown rendering, syntax
 * highlighting and the folding of a turn are all things a unit test can only
 * approximate — what matters is what ends up on the page, and in particular
 * that model output that *looks* like markup does not become any.
 */
import { expect, test } from '@playwright/test';
import {
    STUB,
    PROCESS_OUTPUT,
    call,
    emit,
    modelResponse,
    open,
    playTurn,
    runningSession,
    toolResult,
} from './harness.ts';

test('shows an honest activity cue through a live run', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    const activity = page.getByTestId('run-activity');
    await expect(activity).toHaveCount(0);

    await emit(page, 'run_started', {});
    await expect(activity).toContainText('Waiting for model response');
    await expect(activity.locator('.activity-dot')).toHaveCount(3);

    await emit(page, 'tool_calls', []);
    await expect(activity).toContainText('Running tools');
    await emit(page, 'tool_results', []);
    await expect(activity).toContainText('Waiting for model response');
    await emit(page, 'model_response', modelResponse('Done.'));
    await expect(activity).toContainText('Processing response');
    await emit(page, 'run_finished', { status: 'completed' });
    await expect(activity).toHaveCount(0);
});

test('shows a model failure beside its run with guarded retry guidance', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await emit(page, 'run_started', {});
    await emit(page, 'run_finished', {
        status: 'failed', error: 'HTTP 503 after retries', exchanges: 0,
        failure: { stage: 'model_request', can_continue: true },
    });

    const notice = page.getByTestId('run-failure');
    await expect(notice).toContainText('Model request failed');
    await expect(notice).toContainText('Continue run');
    await expect(notice.locator('pre')).not.toBeVisible();
    await notice.getByText('Technical details').click();
    await expect(notice.locator('pre')).toContainText('HTTP 503 after retries');

    await emit(page, 'input_admitted', { operation: 'continue' },
        { request_id: 'req-cont' });
    await emit(page, 'run_started', {});
    await emit(page, 'model_response', modelResponse('Recovered answer'));
    await emit(page, 'run_finished', { status: 'completed', exchanges: 1 });
    await expect(page.getByTestId('run-failure').first())
        .toContainText('could be continued when it settled');
    await expect(page.getByTestId('run-failure').first()).not.toContainText('Continue run');

    await emit(page, 'run_started', {});
    await emit(page, 'model_response', modelResponse('Partial answer before failure'));
    await emit(page, 'run_finished', {
        status: 'failed', error: 'tool effects require inspection', exchanges: 1,
        failure: { stage: 'other', can_continue: false },
    });
    await expect(page.getByTestId('run-failure').last())
        .toContainText('Inspect the error and worker state');
    await expect(page.getByTestId('run-failure').last()).not.toContainText('Continue run');
    const lastRound = page.getByTestId('round').last();
    const assistantBeforeFailure = await lastRound.evaluate((node) => {
        const assistant = node.querySelector('[data-testid="assistant-message"]');
        const failure = node.querySelector('[data-testid="run-failure"]');
        return Boolean(assistant && failure
            && assistant.compareDocumentPosition(failure) & Node.DOCUMENT_POSITION_FOLLOWING);
    });
    expect(assistantBeforeFailure).toBe(true);
});

test('replays a continuation without a retained request record', async ({ page }) => {
    await open(page);
    await emit(page, 'input_admitted', { operation: 'continue' },
        { request_id: 'req-pruned' });
    await emit(page, 'run_started', {}, { request_id: 'req-pruned' });
    await emit(page, 'model_response', modelResponse('Recovered response'),
        { request_id: 'req-pruned' });
    await emit(page, 'run_finished', { status: 'completed', exchanges: 1 },
        { request_id: 'req-pruned' });
    await page.goto('/?session=demo');

    await expect(page.getByTestId('round-summary').last())
        .toContainText('continued from worker state');
    await expect(page.getByTestId('admitted-placeholder')).toHaveCount(0);
    await expect(page.getByTestId('outbox-item')).toHaveCount(0);
});

test('does not recommend retrying an old failure after a worker replacement', async ({ page }) => {
    await open(page);
    await emit(page, 'run_started', {});
    await emit(page, 'run_finished', {
        status: 'failed', error: 'HTTP 503',
        failure: { stage: 'model_request', can_continue: true },
    });
    await page.goto('/?session=demo');
    await expect(page.getByTestId('run-failure')).toContainText('Continue run');

    await page.request.post(`${STUB}/__stub/sessions`, { data: {
        sessions: [{
            ...runningSession('demo'),
            identity: { state: 'live', worker_id: 'replacement-worker', since: null },
        }],
    } });
    await page.reload();
    await expect(page.getByTestId('run-failure'))
        .toContainText('could be continued when it settled');
    await expect(page.getByTestId('run-failure')).not.toContainText('Continue run');
});

test('recovers worker history and contains long content on a narrow viewport', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true,
        historyTurns: [{ index: 0,
            user: [{ type: 'text', raw: 'earlier user message' }],
            steps: [{ index: 0, content: [{ type: 'text', raw: 'x'.repeat(1200) }],
                tool_calls: 2 }], omitted_steps: 0 }],
    } });
    await emit(page, 'ready', { active: false, capabilities: ['session-history'] });
    await page.setViewportSize({ width: 390, height: 720 });
    await page.goto('/?session=demo');
    await expect(page.getByTestId('history-turn')).toHaveCount(1);
    await expect(page.getByTestId('history-turn')).toContainText('earlier user message');
    await expect(page.getByTestId('history-turn')).toContainText('2 tool calls');
    const overflow = await page.getByTestId('transcript').evaluate((node) =>
        node.scrollWidth > node.clientWidth + 1);
    expect(overflow).toBe(false);
    await page.getByLabel('message').fill('a new message after recovery');
    await page.getByRole('button', { name: 'Send' }).click();
    await expect(page.getByTestId('outbox-item')).toContainText('a new message after recovery');
    await expect(page.getByTestId('outbox-item')).toHaveAttribute('data-state', 'admitted');
});

test('collapses older restored turns while keeping them available', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true,
        historyTurns: Array.from({ length: 5 }, (_, index) => ({
            index, user: [{ type: 'text', raw: `input ${index}` }],
            steps: [{ index: 0, content: [{ type: 'text', raw: `answer ${index}` }],
                tool_calls: 0 }], omitted_steps: 0,
        })),
    } });
    await emit(page, 'ready', { active: false, capabilities: ['session-history'] });
    await page.goto('/?session=demo');
    await expect(page.getByTestId('history-turn')).toHaveCount(5);
    const first = page.getByTestId('history-turn').first();
    await expect(first.getByRole('button')).toHaveAttribute('aria-expanded', 'false');
    await first.getByRole('button').click();
    await expect(first).toContainText('answer 0');
});

test('a history refresh keeps detailed tool cards beside the final reply', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true, historyTurns: [],
    } });
    await emit(page, 'ready', { active: false, capabilities: ['session-history'] });
    await page.goto('/?session=demo');
    await expect.poll(async () => {
        const response = await page.request.get(`${STUB}/__stub/received`);
        return (await response.json()).received.filter(
            (message: { type: string }) => message.type === 'history').length;
    }).toBe(1);
    await expect(page.getByTestId('transcript'))
        .not.toContainText('loading conversation history');
    await emit(page, 'input_admitted', {}, { request_id: 'req-tools' });
    await emit(page, 'run_started', {});
    await emit(page, 'input_committed', {});
    await emit(page, 'model_response', modelResponse(''));
    await emit(page, 'tool_calls', [call('tool-1', 'run_command', { command: 'echo hello' })]);
    await emit(page, 'tool_results', [toolResult('tool-1', 'run_command', PROCESS_OUTPUT)]);
    await emit(page, 'model_response', modelResponse('Final answer after the tool.'));
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyTurns: [{ index: 0,
            user: [{ type: 'text', raw: 'Please run the tool.' }],
            steps: [
                { index: 0, content: [], tool_calls: 1 },
                { index: 1, content: [{ type: 'text', raw: 'Final answer after the tool.' }],
                    tool_calls: 0 },
            ], omitted_steps: 0 }],
    } });
    await emit(page, 'run_finished', { status: 'completed', exchanges: 2 });
    const historyQueries = async () => {
        const response = await page.request.get(`${STUB}/__stub/received`);
        return (await response.json()).received.filter(
            (message: { type: string }) => message.type === 'history').length;
    };
    await expect.poll(historyQueries).toBe(1);
    await expect(page.getByTestId('tool-card')).toHaveCount(1);
    await page.keyboard.press('Alt+Enter');
    await page.getByLabel('command input').fill('Refresh');
    await page.getByLabel('command input').press('Enter');
    await expect(page.getByTestId('restored-user-message'))
        .toContainText('Please run the tool.');
    await expect(page.getByTestId('history-turn')).toHaveCount(0);
    await expect(page.getByTestId('tool-card')).toHaveCount(1);
    await expect(page.getByTestId('tool-card')).toHaveAttribute('data-status', 'ok');
    await expect(page.getByTestId('assistant-message').last())
        .toContainText('Final answer after the tool.');

    await page.reload();
    await expect(page.getByTestId('restored-user-message'))
        .toContainText('Please run the tool.');
    await expect(page.getByTestId('history-turn')).toHaveCount(0);
    await expect(page.getByTestId('tool-card')).toHaveCount(1);
});

test('waits for worker history support before querying an older worker', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: { historyEnabled: true } });
    await emit(page, 'ready', { active: false });
    await page.goto('/?session=demo');
    const queries = async () => {
        const response = await page.request.get(`${STUB}/__stub/received`);
        return (await response.json()).received.filter(
            (message: { type: string }) => message.type === 'history');
    };
    await expect.poll(queries).toHaveLength(0);
    await emit(page, 'status', { active: false, capabilities: ['session-history'] });
    await expect.poll(queries).toHaveLength(1);
});

test('reload recovers history after the capability event leaves replay', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true,
        historyTurns: [{ index: 0, user: [{ type: 'text', raw: 'retained by worker' }],
            steps: [], omitted_steps: 0 }],
    } });
    await emit(page, 'status', { active: false, capabilities: ['session-history'] });
    await page.request.post(`${STUB}/__stub/trim-transcript`, { data: { keep: 0 } });
    await page.goto('/?session=demo');
    await expect(page.getByTestId('history-turn')).toContainText('retained by worker');
});

test('the same worker reconnect refreshes history after its new status', async ({ page }) => {
    await open(page);
    const turn = (raw: string) => ({ index: 0,
        user: [{ type: 'text', raw }], steps: [], omitted_steps: 0 });
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true, historyTurns: [turn('before disconnect')],
    } });
    await emit(page, 'status', { active: false, capabilities: ['session-history'] });
    await page.goto('/?session=demo');
    await expect(page.getByTestId('history-turn')).toContainText('before disconnect');
    const queries = async () => {
        const response = await page.request.get(`${STUB}/__stub/received`);
        return (await response.json()).received.filter(
            (message: { type: string }) => message.type === 'history').length;
    };
    await expect.poll(queries).toBe(1);

    await page.request.post(`${STUB}/__stub/connection`, { data: { connected: false } });
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyTurns: [turn('changed while disconnected')],
    } });
    await page.request.post(`${STUB}/__stub/connection`, { data: { connected: true } });
    await expect.poll(queries).toBe(1);
    await emit(page, 'status', { active: false, capabilities: ['session-history'] });
    await expect.poll(queries).toBe(2);
    await expect(page.getByTestId('history-turn')).toContainText('changed while disconnected');
});

test('completed live runs do not re-fetch the whole worker history', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true,
        historyTurns: Array.from({ length: 25 }, (_, index) => ({
            index, user: [], steps: [], omitted_steps: 0,
        })),
    } });
    await emit(page, 'status', { active: false, capabilities: ['session-history'] });
    await page.goto('/?session=demo');
    const queries = async () => {
        const response = await page.request.get(`${STUB}/__stub/received`);
        return (await response.json()).received.filter(
            (message: { type: string }) => message.type === 'history').length;
    };
    await expect.poll(queries).toBe(3);
    await expect(page.getByTestId('transcript'))
        .not.toContainText('loading conversation history');
    for (let index = 0; index < 3; index += 1) {
        await emit(page, 'run_started', {}, { run_id: `run-${index}` });
        await emit(page, 'model_response', modelResponse(`answer ${index}`),
            { run_id: `run-${index}` });
        await emit(page, 'run_finished', { status: 'completed' },
            { run_id: `run-${index}` });
    }
    await expect(page.getByTestId('assistant-message')).toHaveCount(3);
    expect(await queries()).toBe(3);
});

for (const [name, response] of [
    ['missing revision', { revision: null }],
    ['wrong turn index', { turns: [{ index: 1, user: [], steps: [], omitted_steps: 0 }] }],
    ['wrong step index', { turns: [{ index: 0, user: [], omitted_steps: 0,
        steps: [{ index: 3, content: [], tool_calls: 0 }] }] }],
    ['invalid omitted count', { turns: [{ index: 0, user: [], steps: [],
        omitted_steps: -1 }] }],
] as const) {
    test(`a page with ${name} stops pagination without another request`, async ({ page }) => {
        await open(page);
        await page.request.post(`${STUB}/__stub/settings`, { data: {
            historyEnabled: true,
            historyTurns: [0, 1].map((index) => ({ index, user: [], steps: [],
                omitted_steps: 0 })),
            historyResponses: [{ next: 1, total: 2,
                turns: [{ index: 0, user: [], steps: [], omitted_steps: 0 }],
                ...response }],
        } });
        await emit(page, 'status', { active: false, capabilities: ['session-history'] });
        await page.goto('/?session=demo');
        await expect(page.getByText('Worker returned an invalid conversation history page.'))
            .toBeVisible();
        const received = await page.request.get(`${STUB}/__stub/received`);
        expect((await received.json()).received.filter(
            (message: { type: string }) => message.type === 'history')).toHaveLength(1);
        await expect(page.getByTestId('transcript'))
            .not.toContainText('loading conversation history');
    });
}

test('a history page without request ID ends the pending load', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true,
        historyTurns: [{ index: 0, user: [], steps: [], omitted_steps: 0 }],
        historyResponses: [{ request_id: null }],
    } });
    await emit(page, 'status', { active: false, capabilities: ['session-history'] });
    await page.goto('/?session=demo');
    await expect(page.getByText('Worker returned a history page without a request ID.'))
        .toBeVisible();
    await expect(page.getByTestId('transcript'))
        .not.toContainText('loading conversation history');
});

test('revision change restarts pagination and a bad restart ends loading', async ({ page }) => {
    await open(page);
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true,
        historyTurns: [0, 1].map((index) => ({ index, user: [], steps: [],
            omitted_steps: 0 })),
        historyResponses: [
            { next: 1, total: 2,
                turns: [{ index: 0, user: [], steps: [], omitted_steps: 0 }] },
            { revision: 2 },
            { revision: null },
        ],
    } });
    await emit(page, 'status', { active: false, capabilities: ['session-history'] });
    await page.goto('/?session=demo');
    await expect(page.getByText('Worker returned an invalid conversation history page.'))
        .toBeVisible();
    const received = await page.request.get(`${STUB}/__stub/received`);
    const queries = (await received.json()).received.filter(
        (message: { type: string }) => message.type === 'history');
    expect(queries.map((query: { start: number }) => query.start)).toEqual([0, 1, 0]);
    await expect(page.getByTestId('transcript'))
        .not.toContainText('loading conversation history');
});

test('a pruned history cursor restarts from zero when the revision changes', async ({ page }) => {
    await open(page);
    const turn = (index: number, raw: string) => ({ index,
        user: [{ type: 'text', raw }], steps: [], omitted_steps: 0 });
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true,
        historyTurns: Array.from({ length: 6 }, (_, index) => turn(index, `old ${index}`)),
        historyResponses: [
            { next: 5, total: 6, turns: Array.from({ length: 5 },
                (_, index) => turn(index, `old ${index}`)) },
            { revision: 2, start: 3, next: 3, total: 3, turns: [] },
            { revision: 2, next: 3, total: 3,
                turns: Array.from({ length: 3 }, (_, index) => turn(index, `new ${index}`)) },
        ],
    } });
    await emit(page, 'status', { active: false, capabilities: ['session-history'] });
    await page.goto('/?session=demo');
    await expect(page.getByTestId('history-turn')).toHaveCount(3);
    await expect(page.getByTestId('history-turn').first()).toContainText('new 0');
    const received = await page.request.get(`${STUB}/__stub/received`);
    const queries = (await received.json()).received.filter(
        (message: { type: string }) => message.type === 'history');
    expect(queries.map((query: { start: number }) => query.start)).toEqual([0, 5, 0]);
    await expect(page.getByText('Worker returned an invalid conversation history page.'))
        .toHaveCount(0);
});

test('an invalid partial-turn cursor error retries from zero', async ({ page }) => {
    await open(page);
    const step = { index: 0, content: [{ type: 'text', raw: 'recovered answer' }],
        tool_calls: 0 };
    await page.request.post(`${STUB}/__stub/settings`, { data: {
        historyEnabled: true,
        historyTurns: [{ index: 0, user: [], steps: [step], omitted_steps: 0 }],
        historyResponses: [
            { next: 0, next_step: 1, total: 1,
                turns: [{ index: 0, user: [], steps: [step], omitted_steps: 1 }] },
            { __error: 'history step is outside the selected turn' },
            { revision: 2 },
        ],
    } });
    await emit(page, 'status', { active: false, capabilities: ['session-history'] });
    await page.goto('/?session=demo');
    await expect(page.getByTestId('history-turn')).toContainText('recovered answer');
    const received = await page.request.get(`${STUB}/__stub/received`);
    const queries = (await received.json()).received.filter(
        (message: { type: string }) => message.type === 'history');
    expect(queries.map((query: { start: number; step: number }) =>
        [query.start, query.step])).toEqual([[0, 0], [0, 1], [0, 0]]);
    await expect(page.getByTestId('transcript'))
        .not.toContainText('loading conversation history');
});

test('keeps the compact composer controls aligned and inside a narrow viewport', async ({ page }) => {
    await open(page);
    await page.setViewportSize({ width: 320, height: 568 });
    await page.goto('/?session=demo');
    const message = page.getByLabel('message');
    const composer = page.locator('main > form');
    const attach = page.getByRole('button', { name: 'Attach a reference' });
    const confirmation = page.getByRole('button', { name: /confirmation mode:/ });
    const send = page.getByRole('button', { name: 'Send' });
    const initial = await message.boundingBox();
    expect(initial).not.toBeNull();
    expect(initial!.height).toBeGreaterThanOrEqual(95);
    expect(initial!.height).toBeLessThanOrEqual(97);

    await message.fill('long input '.repeat(180));
    const field = await message.boundingBox();
    const controls = await Promise.all([attach.boundingBox(), confirmation.boundingBox(),
        send.boundingBox()]);
    expect(field).not.toBeNull();
    expect(field!.height).toBeLessThanOrEqual(193);
    for (const control of controls) {
        expect(control).not.toBeNull();
        expect(control!.y).toBeGreaterThanOrEqual(field!.y + field!.height);
        expect(control!.x + control!.width).toBeLessThanOrEqual(320);
        expect(control!.height).toBe(controls[0]!.height);
    }
    expect(controls[1]!.y).toBe(controls[0]!.y);
    expect(controls[2]!.y).toBe(controls[0]!.y);
    const contents = await Promise.all([attach, confirmation, send].map((button) =>
        button.evaluate((node) => {
            const icon = node.querySelector('svg')!.getBoundingClientRect();
            const label = node.querySelector('span')!.getBoundingClientRect();
            return { iconY: icon.y, iconHeight: icon.height,
                labelY: label.y, labelHeight: label.height };
        })));
    for (const content of contents) {
        expect(content.iconY).toBe(contents[0]!.iconY);
        expect(content.iconHeight).toBe(contents[0]!.iconHeight);
        expect(content.labelY).toBe(contents[0]!.labelY);
        expect(content.labelHeight).toBe(contents[0]!.labelHeight);
    }
    const card = message.locator('xpath=..');
    const neutralFocus = await card.evaluate((node) => {
        const sample = document.createElement('div');
        sample.style.borderColor = 'var(--ink-muted)';
        document.body.append(sample);
        const colour = getComputedStyle(sample).borderColor;
        sample.remove();
        return colour;
    });
    await message.focus();
    expect(await card.evaluate((node) => getComputedStyle(node).borderColor)).toBe(neutralFocus);
    expect(await message.evaluate((node) => getComputedStyle(node).outlineStyle)).toBe('none');
    const backgrounds = await page.evaluate(() => ({
        transcript: getComputedStyle(document.querySelector('[data-testid="transcript"]')!).backgroundColor,
        composer: getComputedStyle(document.querySelector('textarea[aria-label="message"]')!.closest('form')!).backgroundColor,
    }));
    // The scroll area inherits its colour from the conversation surface.
    const conversationBackground = await page.locator('#conversation').evaluate(
        (node) => getComputedStyle(node).backgroundColor);
    expect(backgrounds.composer).toBe(conversationBackground);
    expect(await composer.isVisible()).toBe(true);

    const messageHeight = (await composer.boundingBox())!.height;
    await message.press('Alt+Enter');
    await expect(page.getByLabel('command input')).toBeFocused();
    expect((await composer.boundingBox())!.height).toBe(messageHeight);
    const run = page.getByRole('button', { name: 'Run', exact: true });
    const runBounds = await run.boundingBox();
    expect(runBounds).not.toBeNull();
    expect({
        x: runBounds!.x,
        y: runBounds!.y,
        width: runBounds!.width,
        height: runBounds!.height,
    }).toEqual({
        x: controls[2]!.x,
        y: controls[2]!.y,
        width: controls[2]!.width,
        height: controls[2]!.height,
    });
    await expect(page.getByText('Alt + Enter switches modes')).toHaveCount(0);
    await page.getByLabel('command input').press('Alt+Enter');
    await expect(page.getByLabel('message')).toBeFocused();
    expect((await composer.boundingBox())!.height).toBe(messageHeight);

    await emit(page, 'run_started', {});
    await expect(page.getByRole('button', { name: 'Cancel run' })).toBeVisible();
    const activeHeight = (await composer.boundingBox())!.height;
    await page.getByLabel('message').press('Alt+Enter');
    expect((await composer.boundingBox())!.height).toBe(activeHeight);
    await page.getByLabel('command input').press('Alt+Enter');
    expect((await composer.boundingBox())!.height).toBe(activeHeight);
});

test('keeps the activity text and stops its motion when requested', async ({ page }) => {
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await open(page);
    await page.getByTestId('session-row').click();
    await emit(page, 'run_started', {});
    const activity = page.getByTestId('run-activity');
    await expect(activity).toContainText('Waiting for model response');
    const duration = await activity.locator('.activity-dot').first().evaluate(
        (dot) => getComputedStyle(dot).animationDuration,
    );
    expect(Number.parseFloat(duration)).toBeLessThan(0.01);
});

test('renders a model response as markdown', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    await emit(page, 'model_response', modelResponse([
        '# A heading',
        '',
        'A paragraph with **bold**, `code` and a [link](https://example.com/x).',
        '',
        '- one',
        '- two',
        '',
        '| column | value |',
        '| --- | --- |',
        '| a | 1 |',
        '',
        '> quoted',
    ].join('\n')));

    const transcript = page.getByTestId('transcript');
    await expect(transcript.getByRole('heading', { name: 'A heading' })).toBeVisible();
    await expect(transcript.locator('strong', { hasText: 'bold' })).toBeVisible();
    await expect(transcript.locator('li')).toHaveCount(2);
    await expect(transcript.locator('table')).toBeVisible();
    await expect(transcript.locator('blockquote')).toContainText('quoted');
    await expect(transcript.locator('a[href="https://example.com/x"]')).toHaveAttribute(
        'rel', /noreferrer/,
    );
});

test('never turns model output into markup', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    // The whole reason the panel renders markdown through a React tree rather
    // than through HTML: a response is untrusted text.
    await emit(page, 'model_response', modelResponse([
        '<script>window.__pwned = true;</script>',
        '',
        '<img src=x onerror="window.__pwned = true">',
        '',
        '[click me](javascript:window.__pwned=true)',
    ].join('\n')));

    const transcript = page.getByTestId('transcript');
    await expect(transcript).toContainText('<script>');
    await expect(transcript).toContainText('onerror');
    await expect(transcript.locator('script')).toHaveCount(0);
    await expect(transcript.locator('img')).toHaveCount(0);
    // A `javascript:` target is shown, not linked.
    await expect(transcript.locator('a')).toHaveCount(0);
    expect(await page.evaluate(() => (window as unknown as { __pwned?: boolean }).__pwned))
        .toBeUndefined();
});

test('highlights a fenced code block and offers to copy it', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    await emit(page, 'model_response', modelResponse([
        '```js',
        'const answer = 42;',
        '```',
    ].join('\n')));

    const transcript = page.getByTestId('transcript');
    await expect(transcript.getByText('js', { exact: true })).toBeVisible();
    await expect(transcript.locator('pre code .hljs-keyword')).not.toHaveCount(0);
    await expect(transcript.getByRole('button', { name: 'copy' })).toBeVisible();
});

test('hides protocol events until technical details are asked for', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await playTurn(page, { text: 'hello' });

    await expect(page.getByTestId('assistant-message')).toContainText('hello');
    await expect(page.getByTestId('protocol-line')).toHaveCount(0);
    await expect(page.getByTestId('round-summary')).toContainText('protocol event');

    await page.getByTestId('details-toggle').check();

    await expect(page.getByTestId('protocol-line').first()).toBeVisible();
    await expect(page.getByTestId('transcript')).toContainText('run_finished');
    // The payload is one click further in, so the timeline stays a timeline.
    await expect(page.getByTestId('protocol-line').first().locator('details'))
        .not.toHaveAttribute('open', '');
});

test('reveals a tool call and its streams on demand', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    await emit(page, 'input_admitted', {}, { request_id: 'req-1' });
    await emit(page, 'run_started', {});
    await emit(page, 'tool_calls', [
        call('call-1', 'run_command', { command: 'echo hello', expected_runtime_milliseconds: 1000 }),
    ]);
    await emit(page, 'tool_results', [toolResult('call-1', 'run_command', PROCESS_OUTPUT)]);
    await emit(page, 'run_finished', { status: 'completed', exchanges: 1 });

    const card = page.getByTestId('tool-card');
    await expect(card).toHaveCount(1, { timeout: 10_000 });
    await expect(card).toHaveAttribute('data-tool', 'run_command');
    await expect(card).toHaveAttribute('data-status', 'ok');

    const details = card.getByTestId('tool-details');
    await expect(details).not.toHaveAttribute('open', '');
    await expect(card.locator('p[title="echo hello"]')).toBeVisible();
    await expect(card.getByText('stdout', { exact: true })).toBeHidden();
    await details.locator('summary').first().click();

    // The expanded command is highlighted, labelled, and copyable.
    await expect(card.getByText('bash', { exact: true })).toBeVisible();
    await expect(card).toContainText('echo hello');
    // The rest of the arguments are folded away rather than shown as the
    // argument blob the command would otherwise be buried in.
    await expect(card.getByText('other arguments').locator('..')).not.toHaveAttribute('open', '');
    await expect(card.getByText('other arguments')).toBeVisible();

    // The result is read for structure: named streams, not one text blob.
    await expect(card.getByText('stdout', { exact: true })).toBeVisible();
    await expect(card.getByText('stderr', { exact: true })).toBeVisible();
    await expect(card).toContainText('hello');
    await expect(card).toContainText('oops');
    await expect(card).toContainText('the process reported 12ms of runtime');
    // The reported fields are labelled, and labelled as the tool's own report.
    await expect(card).toContainText('exit_code');
    await expect(card).toContainText('0');

    await details.locator('summary').first().click();
    await expect(card.getByText('stdout', { exact: true })).toBeHidden();
});

test('keeps a long edit argument out of the collapsed tool card', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    const replacement = 'replacement-marker-'.repeat(400);
    await emit(page, 'tool_calls', [call('edit-1', 'str_replace_edit', {
        path: '/tmp/example.cpp', old_str: 'old text', new_str: replacement,
    })]);

    const card = page.getByTestId('tool-card');
    await expect(card).toContainText('/tmp/example.cpp');
    expect(await card.innerText()).not.toContain(replacement);
    await card.getByTestId('tool-details').locator('summary').first().click();
    expect(await card.innerText()).toContain(replacement);
});

test('draws one card for a batch the response and the event both describe', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    const proposed = call('call-1', 'run_command', { command: 'ls' });
    await emit(page, 'input_admitted', {}, { request_id: 'req-1' });
    await emit(page, 'model_response', modelResponse('', { invokes: [proposed] }));
    await emit(page, 'tool_calls', [proposed]);
    await emit(page, 'run_finished', { status: 'completed' });

    // The old panel drew a card for the response's calls and another for the
    // batch, so one command appeared twice.
    await expect(page.getByTestId('tool-card')).toHaveCount(1);
});

test('folds old turns and opens them on demand', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    for (const text of ['first turn', 'second turn', 'third turn', 'fourth turn', 'fifth turn']) {
        await emit(page, 'model_response', modelResponse(text));
        await emit(page, 'input_admitted', {}, { request_id: `req-${text}` });
        await emit(page, 'run_finished', { status: 'completed' });
    }

    const summaries = page.getByTestId('round-summary');
    await expect(summaries).toHaveCount(5);
    // Only the most recent turns are open, but every one is still on the page.
    await expect(page.getByTestId('transcript')).toContainText('fifth turn');
    await expect(page.getByTestId('transcript')).not.toContainText('first turn');
    await expect(summaries.first()).toContainText('turn 1');

    await summaries.first().click();
    await expect(page.getByTestId('transcript')).toContainText('first turn');

    // And closing it again leaves the summary, not a hole.
    await summaries.first().click();
    await expect(page.getByTestId('transcript')).not.toContainText('first turn');
    await expect(summaries.first()).toBeVisible();
});

test('folds a long message and keeps the whole of it', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    const long = 'x'.repeat(900);
    await page.getByLabel('message').fill(long);
    await page.getByRole('button', { name: 'Send' }).click();

    const bubble = page.getByTestId('outbox-item');
    await expect(bubble).toContainText('show all 900 characters');
    await bubble.getByRole('button', { name: /show all/ }).click();
    await expect(bubble).toContainText(long);
});

test('a folded turn still shows what went wrong in it', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();

    // Two turns, so the first one is folded away.
    await emit(page, 'input_admitted', {}, { request_id: 'req-1' });
    await emit(page, 'error', { message: 'the storage layer refused the write' });
    await emit(page, 'run_finished', { status: 'failed' });
    for (const text of ['b', 'c', 'd']) {
        await emit(page, 'model_response', modelResponse(text));
        await emit(page, 'input_admitted', {}, { request_id: `req-${text}` });
        await emit(page, 'run_finished', { status: 'completed' });
    }

    // A problem must not be hidden by the fold that hides its turn's machinery.
    await expect(page.getByTestId('transcript-problem')).toContainText(
        'the storage layer refused the write',
    );
});
