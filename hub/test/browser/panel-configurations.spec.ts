import { expect, test } from '@playwright/test';
import { readFile } from 'node:fs/promises';
import { open } from './harness.ts';

test('manage worker files through templates, external editing, downloads and revision-checked uploads', async ({ page }) => {
    const template = '# Comment\nmodel: example\n';
    const files = new Map<string, { text: string; revision: string }>();
    const requests: string[] = [];
    await page.route('**/api/configurations', route => route.fulfill({ json: {
        launch: ['local'], worker: [...files.keys()], editorAvailable: true,
    } }));
    await page.route('**/api/configurations/worker/**', async route => {
        const req = route.request();
        const [, , , , id, action] = new URL(req.url()).pathname.split('/');
        const saved = files.get(id!);
        if (id === 'template' && !action) return route.fulfill({ json: { text: template } });
        if (action === 'open') {
            requests.push(id!);
            return route.fulfill({ json: { opened: true } });
        }
        if (action === 'source' && saved) return route.fulfill({ json: { ...saved, id, kind: 'worker', path: '/tmp/' + id } });
        if (req.method() === 'PUT') {
            const body = req.postDataJSON();
            if ((saved?.revision ?? null) !== body.revision) return route.fulfill({ status: 409, json: { error: 'bad_request', message: 'Configuration changed; reload before saving' } });
            const record = { text: body.text, revision: saved ? saved.revision + 'x' : 'r1' };
            files.set(id!, record);
            return route.fulfill({ json: { ...record, id, kind: 'worker' } });
        }
        return route.continue();
    });
    await open(page);
    await page.getByRole('button', { name: 'Configurations', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: 'Configurations' });
    await dialog.getByLabel('Configuration name').fill('research');
    await dialog.getByRole('button', { name: 'New from template' }).click();
    await expect(dialog.getByRole('status')).toContainText('Saved research.yaml');
    await dialog.getByRole('button', { name: 'Open in editor' }).click();
    await expect(dialog.getByRole('status')).toContainText('Requested editor');
    expect(requests).toEqual(['research']);
    const received = page.waitForEvent('download');
    await dialog.getByRole('button', { name: 'Download', exact: true }).click();
    const download = await received;
    expect(download.suggestedFilename()).toBe('research.yaml');
    expect(await readFile((await download.path())!, 'utf8')).toBe(template);
    const updated = { name: 'local.yaml', mimeType: 'text/plain', buffer: Buffer.from(template + '# Edited locally\r\n') };
    page.once('dialog', message => message.accept());
    await dialog.locator('input[type=file][aria-label="Replace from file"]').setInputFiles(updated);
    await expect.poll(() => files.get('research')?.text).toBe(updated.buffer.toString());
    files.set('research', { text: 'edited independently', revision: 'new' });
    page.once('dialog', message => message.accept());
    await dialog.locator('input[type=file][aria-label="Replace from file"]').setInputFiles(updated);
    await expect(dialog.getByRole('alert')).toContainText('reload before saving');
    expect(files.get('research')?.text).toBe('edited independently');
    await dialog.locator('input[type=file][aria-label="Upload new file"]').setInputFiles(updated);
    await expect(dialog.getByRole('status')).toContainText('Saved local.yaml');
    expect(files.get('local')?.text).toBe(updated.buffer.toString());
});

test('session creation offers both persisted configuration selectors', async ({ page }) => {
    await open(page);
    await page.getByRole('button', { name: 'Create a session', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: 'Create a session' });
    await expect(dialog.getByLabel('Launch configuration')).toHaveValue('local');
    await dialog.getByLabel('Launch configuration').focus();
    expect(await dialog.getByLabel('Launch configuration').evaluate(node => getComputedStyle(node).outlineStyle)).toBe('none');
    await expect(dialog.getByLabel('Worker configuration')).toHaveValue('default');
    await expect(dialog.getByRole('button', { name: 'Create session', exact: true })).toBeEnabled();
});

test('file manager fits a narrow viewport without a configured host editor', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await page.route('**/api/configurations/worker/default/source', route => route.fulfill({ json: {
        kind: 'worker', id: 'default', text: 'driver_model: sample', revision: 'r1', path: '/tmp/default.yaml',
    } }));
    await open(page);
    await page.getByRole('button', { name: 'Open the session list' }).click();
    await page.getByRole('button', { name: 'Configurations', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: 'Configurations' });
    await dialog.getByLabel('Saved configuration').selectOption('default');
    await expect(dialog.getByRole('button', { name: 'Download', exact: true })).toBeVisible();
    await expect(dialog.getByRole('button', { name: 'Open in editor' })).toHaveCount(0);
    const bounds = (await dialog.boundingBox())!;
    expect(bounds.x).toBeGreaterThanOrEqual(0);
    expect(bounds.x + bounds.width).toBeLessThanOrEqual(390);
    expect(bounds.y + bounds.height).toBeLessThanOrEqual(844);
});
