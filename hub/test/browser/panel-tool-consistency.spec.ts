import { expect, test } from '@playwright/test';
import { call, emit, modelResponse, open, toolResult } from './harness.ts';

test('legacy tools have one card per exchange through live updates and replay', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    await emit(page, 'run_started', {});
    for (const name of ['first', 'second']) {
        const args = { command: `echo ${name}` };
        const proposal = call('', 'run_command', args);
        await emit(page, 'model_response', modelResponse('', { invokes: [proposal] }));
        await emit(page, 'tool_calls', [proposal]);
        const result = toolResult('', 'run_command', `Output ${name}`);
        result.invoke_return.query.arguments = args;
        await emit(page, 'tool_results', [result]);
        await expect(page.getByTestId('tool-card')).toHaveCount(name === 'first' ? 1 : 2);
    }
    await emit(page, 'run_finished', { status: 'completed', exchanges: 2 });
    await page.reload();
    const cards = page.getByTestId('tool-card');
    await expect(cards).toHaveCount(2);
    for (const [index, name] of ['first', 'second'].entries()) {
        await expect(cards.nth(index)).toHaveAttribute('data-status', 'ok');
        await cards.nth(index).getByTestId('tool-details').locator('summary').first().click();
        await expect(cards.nth(index)).toContainText(`Output ${name}`);
        await expect(cards.nth(index)).not.toContainText(`Output ${name === 'first' ? 'second' : 'first'}`);
    }
});

test('ambiguous legacy results explicitly remain separate from proposed calls', async ({ page }) => {
    await open(page);
    await page.getByTestId('session-row').click();
    const proposals = [call('', 'run_command', { command: 'first' }),
        call('', 'run_command', { command: 'second' })];
    await emit(page, 'model_response', modelResponse('', { invokes: proposals }));
    await emit(page, 'tool_calls', proposals);
    await emit(page, 'tool_results', [{ query: { name: 'run_command' },
        output: { type: 'text', raw: 'Unmatched output' } }]);
    await emit(page, 'run_finished', { status: 'completed' });
    const cards = page.getByTestId('tool-card');
    await expect(cards).toHaveCount(3);
    await expect(cards.nth(0)).toHaveAttribute('data-status', 'unknown');
    await expect(cards.nth(1)).toHaveAttribute('data-status', 'unknown');
    await cards.nth(2).getByTestId('tool-details').locator('summary').first().click();
    await expect(cards.nth(2)).toContainText('Unmatched output');
    await expect(cards.nth(2)).toContainText('No unambiguous call match was retained');
});

test('duplicate output labels retain separate fields and independent disclosure state', async ({ page }) => {
    const keyWarnings: string[] = [];
    page.on('console', message => {
        if (/same key|unique.*key/i.test(message.text())) keyWarnings.push(message.text());
    });
    await open(page);
    await page.getByTestId('session-row').click();
    await emit(page, 'tool_calls', [call('output', 'run_command', {})]);
    const first = `first ${'a'.repeat(1300)}`;
    const second = `second ${'b'.repeat(1300)}`;
    const output = `[[hint]]: first hint\n[[hint]]: second hint\n\n`
        + `stdout (${first.length} bytes):\n${first}\n\n`
        + `stdout (${second.length} bytes):\n${second}\n`;
    await emit(page, 'tool_results', [toolResult('output', 'run_command', output)]);
    const card = page.getByTestId('tool-card');
    await card.getByTestId('tool-details').locator('summary').first().click();
    await expect(card.locator('dl dt')).toHaveText(['hint', 'hint']);
    await expect(card.locator('dl dd')).toHaveText(['first hint', 'second hint']);
    await expect(card.getByRole('button', { name: 'expand all' })).toHaveCount(2);
    await card.getByRole('button', { name: 'expand all' }).first().click();
    await expect(card.getByText(first, { exact: true })).toBeVisible();
    await expect(card.getByText(second, { exact: true })).toHaveCount(0);
    await emit(page, 'run_finished', { status: 'completed' });
    await card.getByRole('button', { name: 'expand all' }).click();
    await expect(card.getByText(second, { exact: true })).toBeVisible();
    await card.getByRole('button', { name: 'collapse' }).first().click();
    await expect(card.getByText(first, { exact: true })).toHaveCount(0);
    await expect(card.getByText(second, { exact: true })).toBeVisible();
    expect(keyWarnings).toEqual([]);
});
