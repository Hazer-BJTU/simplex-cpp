import { expect, test } from '@playwright/test';
import { STUB, emit, modelResponse, open } from './harness.ts';
import { normalizeDisplay } from '../../src/protocol/display.ts';
import { SessionTranscript } from '../../src/state/transcript.ts';

const literal = '# reasoning heading\n```html\n<b>literal</b>\n```\nhttps://example.com\n中文🌍';

test('reasoning is lazy literal text in live and replayed answers, without Markdown work', async ({ page }) => {
    await open(page, '?session=demo&panel_profile=1');
    await emit(page, 'run_started', {});
    await emit(page, 'model_response', modelResponse('Final answer', {
        reasoning: { type: 'text', modality: 'text', raw: literal, truncated: true, bytes: 3000000 },
    }));
    const answer = page.getByTestId('assistant-message');
    await expect(answer).toContainText('Final answer');
    await expect(page.getByTestId('reasoning-text')).toHaveCount(0);
    await page.evaluate(() => (window as unknown as { __simplexPanelProfile: { reset(): void } }).__simplexPanelProfile.reset());
    await answer.locator('summary').filter({ hasText: 'reasoning' }).click();
    await expect(page.getByTestId('reasoning-text')).toHaveText(literal);
    await expect(page.getByTestId('reasoning-text').locator('h1, a, pre, code, .hljs')).toHaveCount(0);
    expect(await page.evaluate(() => (window as unknown as {
        __simplexPanelProfile: { read(): Record<string, { count: number }> }
    }).__simplexPanelProfile.read().markdown?.count ?? 0)).toBe(0);
    await expect(answer).toContainText('Reasoning preview');
    await page.reload();
    await expect(page.getByTestId('reasoning-text')).toHaveCount(0);
    await page.getByTestId('assistant-message').locator('summary').filter({ hasText: 'reasoning' }).click();
    await expect(page.getByTestId('reasoning-text')).toHaveText(literal);
});

test('restored history uses the same literal renderer and explicit reasoning marker', async ({ page }) => {
    await open(page, '?session=demo');
    await page.request.post(`${STUB}/__stub/settings`, { data: { historyEnabled: true, historyTurns: [{
        index: 0, user: [{ type: 'text', modality: 'text', raw: 'Old task' }],
        steps: [{ index: 0, tool_calls: 0, content: [{ type: 'text', modality: 'text', raw: 'Old answer' }],
            reasoning: { type: 'text', modality: 'text', raw: literal, truncated: true, bytes: 9000 } }], omitted_steps: 0,
    }] } });
    await emit(page, 'ready', { active: false, capabilities: ['session-history'] });
    await page.reload();
    const turn = page.getByTestId('history-turn');
    await expect(turn).toContainText('Old answer');
    await turn.locator('summary').filter({ hasText: 'reasoning' }).click();
    await expect(page.getByTestId('reasoning-text')).toHaveText(literal);
    await expect(turn).toContainText('Reasoning preview of 9000 UTF-8 bytes');
});

test('answer navigation validates offsets and retains only one exact page', async ({ page }) => {
    const source = { worker_id: 'stub-worker', turn: 0, step: 0, commit_sequence: '1' };
    await open(page, '?session=demo');
    await page.route('**/api/sessions/demo/answer', async route => {
        const query = route.request().postDataJSON();
        expect(query.source).toEqual(source);
        const raw = query.offset === 0 ? 'first page 中文' : 'last page 🌍';
        const length = new TextEncoder().encode(raw).length;
        const done = query.offset !== 0;
        await route.fulfill({ json: { ...query, request_id: 'reply', raw, type: 'text', modality: 'text',
            bytes: 31, next_offset: query.offset + length, next_part: done ? 1 : 0, total_parts: 1, done } });
    });
    await emit(page, 'run_started', {});
    await emit(page, 'model_response', modelResponse('preview', {
        content: [{ type: 'text', modality: 'text', raw: 'preview', truncated: true, bytes: 31 }], answer_source: source,
    }));
    await page.getByRole('button', { name: 'Read complete answer in pages' }).click();
    await expect(page.getByTestId('answer-pages')).toContainText('first page 中文');
    await page.getByRole('button', { name: 'Next answer page' }).click();
    await expect(page.getByTestId('answer-pages')).toContainText('last page 🌍');
    await expect(page.getByTestId('answer-pages')).not.toContainText('first page 中文');
    await expect(page.getByTestId('answer-pages')).toContainText('End of answer');
});

test('large exact answers mount one plain-text section without losing Unicode or the tail', async ({ page }) => {
    await open(page, '?session=demo');
    const text = 'x'.repeat(32767) + '🌍' + 'y'.repeat(4000) + 'Complete final tail';
    await emit(page, 'run_started', {});
    await emit(page, 'model_response', modelResponse(text));
    const answer = page.getByTestId('large-answer');
    await expect(answer).toContainText('section 1/2');
    await expect(answer).not.toContainText('Complete final tail');
    await expect(answer.locator('pre')).toHaveText('x'.repeat(32767));
    await answer.getByRole('button', { name: 'Next section', exact: true }).click();
    await expect(answer.locator('pre')).toHaveText('🌍' + 'y'.repeat(4000) + 'Complete final tail');
    await expect(answer.getByRole('button', { name: 'Next section', exact: true })).toBeDisabled();
    await answer.getByRole('button', { name: 'Previous section', exact: true }).click();
    await expect(answer.locator('pre')).toHaveText('x'.repeat(32767));
});

test('complex tool previews keep proposal, confirmation and result cards correlated', async ({ page }) => {
    await open(page, '?session=demo');
    const nodes = Array.from({ length: 64 }, (_, index) => ({ first: index, second: index }));
    const calls = ['structured_tool', 'later_tool'].map((name, index) => ({
        arguments: { nodes }, extras: { metadata: { nodes } }, id: `call-${index}`, name,
        type: 'serial_write', security: 'require_confirm',
    }));
    await emit(page, 'run_started', {});
    await emit(page, 'model_response', normalizeDisplay('model_response', modelResponse('', { invokes: calls })));
    await emit(page, 'tool_calls', normalizeDisplay('tool_calls', calls));
    await expect(page.getByTestId('tool-card')).toHaveCount(2);
    await page.request.post(`${STUB}/__stub/confirm`, { data: { confirmation_id: 'structured-approval', call: calls[0] } });
    await expect(page.getByRole('dialog')).toBeVisible();
    await page.keyboard.press('Escape');
    const first = page.locator('[data-testid="tool-card"][data-tool="structured_tool"]');
    const later = page.locator('[data-testid="tool-card"][data-tool="later_tool"]');
    await expect(first).toHaveAttribute('data-status', 'pending');
    await expect(later).toHaveAttribute('data-status', 'running');
    await page.request.post(`${STUB}/__stub/settle`, { data: { confirmation_id: 'structured-approval' } });
    await emit(page, 'tool_results', normalizeDisplay('tool_results', calls.map((query, index) => ({
        invoke_return: { query, output: { type: 'text', modality: 'text', raw: `output-${index}` },
            extras: index === 0 ? { aaa_metadata: { nodes }, error: { stage: 'invoke', message: 'distinct failure' } } : {} },
        type: 'invoke_return', role: 'tool',
    }))));
    await expect(page.getByTestId('tool-card')).toHaveCount(2);
    await expect(first).toHaveAttribute('data-status', 'failed');
    await expect(later).toHaveAttribute('data-status', 'ok');
    await first.locator('summary').first().click();
    await expect(first).toContainText('distinct failure');
    await expect(first).toContainText('output-0');
    await later.locator('summary').first().click();
    await expect(later).toContainText('output-1');
    await expect(later).not.toContainText('output-0');
});

test('whole-answer omission keeps explicit display and exact page access live and after replay', async ({ page }) => {
    await open(page, '?session=demo');
    const source = { worker_id: 'stub-worker', turn: 0, step: 0, commit_sequence: '1', fingerprint: 'a'.repeat(64) };
    const text = 'x'.repeat(70000);
    const transcript = new SessionTranscript({ sessionId: 'demo', limit: 10, byteLimit: 64 * 1024 });
    const projected = transcript.append<{ event: string; data: unknown }>({
        event: 'model_response', data: modelResponse(text, { answer_source: source }),
    });
    expect((projected.data as { display_omitted: boolean }).display_omitted).toBe(true);
    const sources: unknown[] = [];
    await page.route('**/api/sessions/demo/answer', async route => {
        const query = route.request().postDataJSON();
        sources.push(query.source);
        const raw = text.slice(query.offset, query.offset + 32768);
        const end = query.offset + raw.length;
        await route.fulfill({ json: { ...query, request_id: 'reply', raw, type: 'text', modality: 'text',
            bytes: text.length, next_offset: end, next_part: end === text.length ? 1 : 0,
            total_parts: 1, done: end === text.length } });
    });
    await emit(page, 'run_started', {});
    await emit(page, 'model_response', projected.data);
    for (let attempt = 0; attempt < 2; attempt++) {
        const answer = page.getByTestId('assistant-message');
        await expect(answer).toContainText('Response display omitted to fit the display budget.');
        await expect(answer).not.toContainText('(no text in this response)');
        await answer.getByRole('button', { name: 'Read complete answer in pages' }).click();
        let recovered = '';
        for (let index = 0; index < 3; index++) {
            const offset = index * 32768;
            await expect(answer.getByTestId('answer-pages').locator('pre')).toHaveText(text.slice(offset, offset + 32768));
            recovered += await answer.getByTestId('answer-pages').locator('pre').textContent();
            if (index < 2) await answer.getByRole('button', { name: 'Next answer page' }).click();
        }
        expect(recovered).toBe(text);
        await expect(answer.getByTestId('answer-pages')).toContainText('End of answer');
        if (attempt === 0) await page.reload();
    }
    expect(sources).toHaveLength(6);
    expect(sources.every(value => JSON.stringify(value) === JSON.stringify(source))).toBe(true);
});

test('whole-body normalization omission still exposes its source or honest unavailability', async ({ page }) => {
    await open(page, '?session=demo');
    const source = { worker_id: 'stub-worker', turn: 0, step: 0, commit_sequence: '1' };
    // Force the normalizer's aggregate fallback through an indivisible identity,
    // rather than the transcript's smaller retention budget.
    const value = normalizeDisplay('model_response', modelResponse('answer', {
        answer_source: source, invokes: [{ id: 'x'.repeat(800 * 1024), name: 'tool' }],
    })) as { display_omitted: boolean; answer_source?: unknown };
    expect(value.display_omitted).toBe(true);
    await emit(page, 'run_started', {});
    await emit(page, 'model_response', value);
    await expect(page.getByRole('button', { name: 'Read complete answer in pages' })).toBeVisible();
    await emit(page, 'model_response', { display_omitted: true });
    await expect(page.getByTestId('assistant-message').last()).toContainText('complete text is unavailable from this worker');
});

test('100-entry worker batches show only real tools and accurate omission notices after replay', async ({ page }) => {
    await open(page, '?session=demo');
    const calls = Array.from({ length: 100 }, (_, index) => ({ id: `batch-call-${index}`, name: `batch_tool_${index}`,
        arguments: {}, type: 'read_only', security: 'trusted' }));
    const marker = { display_omitted: true, omitted_items: 36 };
    const workerCalls = [...calls.slice(0, 64), marker];
    const workerResults = [...calls.slice(0, 64).map(query => ({ query,
        output: { type: 'text', modality: 'text', raw: `output ${query.id}` } })), marker];
    await emit(page, 'run_started', {});
    await emit(page, 'model_response', normalizeDisplay('model_response', modelResponse('Proposing the batch.', { invokes: workerCalls })));
    await emit(page, 'tool_calls', normalizeDisplay('tool_calls', workerCalls));
    await expect(page.getByTestId('tool-card')).toHaveCount(64);
    await expect(page.getByTestId('tool-omission')).toHaveCount(1);
    await expect(page.getByTestId('tool-omission')).toHaveText('36 tool call previews omitted from this display.');
    await emit(page, 'tool_results', normalizeDisplay('tool_results', workerResults));
    await emit(page, 'run_finished', { status: 'completed' });
    for (let attempt = 0; attempt < 2; attempt++) {
        await expect(page.getByTestId('tool-card')).toHaveCount(64);
        await expect(page.locator('[data-testid="tool-card"][data-status="ok"]')).toHaveCount(64);
        await expect(page.getByTestId('tool-omission')).toHaveText([
            '36 tool call previews omitted from this display.',
            '36 tool result previews omitted from this display.',
        ]);
        await expect(page.getByTestId('transcript')).not.toContainText('(unnamed');
        if (attempt === 0) await page.reload();
    }
});
