import { expect, test } from '@playwright/test';
import { open } from './harness.ts';

test('configuration editor creates from a template, preserves source and reports save conflicts', async ({ page }) => {
    const template = '# Provider comment\ndriver_model: deepseek\nproviders:\n  deepseek:\n    model: deepseek-flash\n';
    let saved: { text: string; revision: string } | null = null;
    await page.route('**/api/configurations/**', async route => {
        const request = route.request();
        const path = new URL(request.url()).pathname;
        if (path.endsWith('/template')) return route.fulfill({ json: { text: template } });
        if (path.endsWith('/validate')) return route.fulfill({ json: { valid: true } });
        if (request.method() === 'PUT') {
            const body = request.postDataJSON() as { text: string; revision: string | null };
            if (saved) return route.fulfill({ status: 409, json: { error: 'conflict', message: 'Configuration changed; reload before saving' } });
            saved = { text: body.text, revision: 'revision-1' };
            return route.fulfill({ json: { ...saved, id: 'custom', kind: 'worker' } });
        }
        return route.continue();
    });
    await open(page);
    await page.getByRole('button', { name: 'Configurations', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: 'Configurations' });
    await dialog.getByRole('button', { name: 'New from template' }).click();
    const editor = dialog.getByRole('textbox', { name: 'Configuration source' });
    await expect(editor).toHaveValue(template);
    await dialog.getByRole('textbox', { name: 'Configuration name' }).fill('custom');
    await editor.fill(template + '# Edited in browser\n');
    await dialog.getByRole('button', { name: 'Save', exact: true }).click();
    await expect(dialog.getByRole('status')).toContainText('Saved.');
    expect(saved!.text).toContain('# Edited in browser');
    await editor.fill(template + '# Newer edit\n');
    await dialog.getByRole('button', { name: 'Save', exact: true }).click();
    await expect(dialog.getByRole('alert')).toContainText('reload before saving');
    await expect(editor).toHaveValue(template + '# Newer edit\n');
    page.once('dialog', message => message.dismiss());
    await dialog.getByRole('button', { name: 'Close', exact: true }).click();
    await expect(dialog).toBeVisible();
    page.once('dialog', message => message.accept());
    await dialog.getByRole('button', { name: 'Close', exact: true }).click();
    await expect(dialog).toBeHidden();
});

test('session creation offers both persisted configuration selectors', async ({ page }) => {
    await open(page);
    await page.getByRole('button', { name: 'Create a session', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: 'Create a session' });
    await expect(dialog.getByLabel('Launch configuration')).toHaveValue('local');
    await expect(dialog.getByLabel('Worker configuration')).toHaveValue('default');
    await expect(dialog.getByRole('button', { name: 'Create session', exact: true })).toBeEnabled();
});

test('configuration source remains text and the editor stays inside a narrow viewport', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    const source = '# <img src=x onerror="window.configInjected=true">\nclient:\n  endpoint: "{{hub.events_endpoint}}"\n';
    await page.route('**/api/configurations/worker/template*', route => route.fulfill({ json: { text: source } }));
    await open(page);
    await page.getByRole('button', { name: 'Open the session list' }).click();
    await page.getByRole('button', { name: 'Configurations', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: 'Configurations' });
    await dialog.getByRole('button', { name: 'New from template' }).click();
    await expect(dialog.getByRole('textbox', { name: 'Configuration source' })).toHaveValue(source);
    await expect(dialog.locator('[data-testid="config-editor"] img')).toHaveCount(0);
    await expect(dialog.locator('.hljs-comment')).toContainText('onerror');
    const bounds = (await dialog.boundingBox())!;
    expect(bounds.x).toBeGreaterThanOrEqual(0);
    expect(bounds.x + bounds.width).toBeLessThanOrEqual(390);
    expect(bounds.y).toBeGreaterThanOrEqual(0);
    expect(bounds.y + bounds.height).toBeLessThanOrEqual(844);
});
