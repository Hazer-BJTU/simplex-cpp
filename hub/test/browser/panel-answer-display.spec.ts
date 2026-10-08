import { expect, test } from '@playwright/test';
import { STUB, emit, modelResponse, open } from './harness.ts';

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
