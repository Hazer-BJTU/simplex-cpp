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
    PROCESS_OUTPUT,
    call,
    emit,
    modelResponse,
    open,
    playTurn,
    toolResult,
} from './harness.ts';

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

test('shows a tool call as a command and its result as streams', async ({ page }) => {
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

    // The command reads as a command: highlighted, labelled, copyable.
    await expect(card.getByText('bash', { exact: true })).toBeVisible();
    await expect(card).toContainText('echo hello');
    // The rest of the arguments are folded away rather than shown as the
    // argument blob the command would otherwise be buried in.
    await expect(card.locator('details').first()).not.toHaveAttribute('open', '');
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
