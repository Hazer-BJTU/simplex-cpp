import { expect, test, type Page } from '@playwright/test';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { defaultConfig, hubRoot } from '../../src/config.ts';
import { createHub } from '../../src/hub.ts';
import { createLogger } from '../../src/log.ts';
import { open } from './harness.ts';

async function rowAction(page: Page, filename: string, action: string) {
    await page.getByRole('button', { name: `Actions for ${filename}`, exact: true }).click();
    await page.getByRole('menuitem', { name: action, exact: true }).click();
}

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
    await dialog.getByRole('button', { name: 'Add worker configuration' }).click();
    await page.getByRole('menuitem', { name: 'New from template…' }).click();
    await dialog.getByRole('textbox', { name: 'New worker configuration' }).fill('research');
    await dialog.getByRole('button', { name: 'Create', exact: true }).click();
    await expect(dialog.getByRole('status')).toContainText('Saved research.yaml');
    await dialog.getByRole('button', { name: 'Open research.yaml in editor' }).click();
    await expect(dialog.getByRole('status')).toContainText('Requested editor');
    expect(requests).toEqual(['research']);
    const received = page.waitForEvent('download');
    await rowAction(page, 'research.yaml', 'Download');
    const download = await received;
    expect(download.suggestedFilename()).toBe('research.yaml');
    expect(await readFile((await download.path())!, 'utf8')).toBe(template);
    const updated = { name: 'local.yaml', mimeType: 'text/plain', buffer: Buffer.from(template + '# Edited locally\r\n') };
    await rowAction(page, 'research.yaml', 'Replace from file…');
    const replacement = dialog.getByRole('form', { name: 'Replace research.yaml' });
    await replacement.locator('input[type=file]').setInputFiles(updated);
    await replacement.getByRole('button', { name: 'Replace', exact: true }).click();
    await expect.poll(() => files.get('research')?.text).toBe(updated.buffer.toString());
    files.set('research', { text: 'edited independently', revision: 'new' });
    // Opening another menu, refreshing, or reopening the library must not rebase the local copy.
    await dialog.getByRole('button', { name: 'Refresh configurations' }).click();
    await dialog.getByRole('button', { name: 'Close', exact: true }).click();
    await page.getByRole('button', { name: 'Configurations', exact: true }).click();
    await rowAction(page, 'research.yaml', 'Replace from file…');
    await replacement.locator('input[type=file]').setInputFiles(updated);
    await replacement.getByRole('button', { name: 'Replace', exact: true }).click();
    await expect(dialog.getByRole('alert')).toContainText('Download the current file');
    expect(files.get('research')?.text).toBe('edited independently');
    await replacement.getByRole('button', { name: 'Cancel' }).click();
    await dialog.getByRole('button', { name: 'Add worker configuration' }).click();
    const chooser = page.waitForEvent('filechooser');
    await page.getByRole('menuitem', { name: 'Upload file…' }).click();
    await (await chooser).setFiles(updated);
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

test('long library rows keep their actions visible on a narrow screen without fetching file contents', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 600 });
    const long = 'configuration-'.repeat(9);
    const files = [...Array.from({ length: 35 }, (_, i) => 'worker-' + i), long];
    await page.route('**/api/configurations', route => route.fulfill({ json: { worker: files, launch: ['local'] } }));
    let sourceReads = 0;
    await page.route('**/api/configurations/**/source', route => { sourceReads++; return route.fulfill({ json: {} }); });
    await open(page);
    await page.getByRole('button', { name: 'Open the session list' }).click();
    await page.getByRole('button', { name: 'Configurations', exact: true }).click();
    const dialog = page.getByRole('dialog', { name: 'Configurations' });
    await expect(dialog.getByRole('button', { name: 'Download worker-0.yaml', exact: true })).toBeVisible();
    await expect(dialog.getByRole('button', { name: / in editor$/ })).toHaveCount(0);
    const actions = dialog.getByRole('button', { name: `Actions for ${long}.yaml` });
    await actions.scrollIntoViewIfNeeded();
    await expect(actions).toBeInViewport({ ratio: 1 });
    await expect(dialog.getByRole('button', { name: 'Close', exact: true })).toBeInViewport({ ratio: 1 });
    expect(await dialog.getByTestId('dialog-body').evaluate(node => node.scrollWidth <= node.clientWidth)).toBe(true);
    await actions.click();
    await expect(page.getByRole('menuitem', { name: 'Rename…' })).toBeInViewport();
    await page.keyboard.press('Escape');
    await expect(actions).toBeFocused();
    await page.keyboard.press('Escape');
    await expect(page.getByRole('button', { name: 'Configurations', exact: true })).toBeFocused();
    expect(sourceReads).toBe(0);
});

test('row actions manage same-named files independently of the session selection', async ({ page }, testInfo) => {
    const config = defaultConfig();
    config.dataDir = await mkdtemp(join(tmpdir(), 'simplex-file-list-'));
    config.listen.port = config.toolRequests.port = 0;
    const hub = createHub({ config, hubRoot, log: createLogger({ level: 'silent' }) });
    try {
        const { url } = await hub.start();
        const created = await page.request.post(url + '/api/sessions', { data: {
            session: 'review', spec: { launchConfig: 'local', workerConfig: 'default' },
        } });
        expect(created.status()).toBe(201);
        await page.emulateMedia({ colorScheme: 'light' });
        await page.goto(url + '/?session=review');
        await page.getByRole('button', { name: 'Configurations', exact: true }).click();
        const dialog = page.getByRole('dialog', { name: 'Configurations' });
        const session = dialog.locator('details[aria-label="Session configuration"]');
        await expect(session.locator('summary')).toContainText('review');
        await expect(dialog).toHaveCSS('width', '640px');
        // A copy and a fresh launch template can share an ID in their respective groups.
        await rowAction(page, 'default.yaml', 'Create copy…');
        const copy = dialog.getByRole('form', { name: 'Create a copy of default.yaml' });
        await copy.getByRole('textbox').fill('research');
        await copy.getByRole('button', { name: 'Create copy' }).click();
        await expect(dialog.getByRole('button', { name: 'Download research.yaml' })).toBeVisible();
        await dialog.getByRole('button', { name: 'Add launch configuration' }).click();
        await page.getByRole('menuitem', { name: 'New from template…' }).click();
        const launchForm = dialog.getByRole('form', { name: 'New launch configuration' });
        await launchForm.getByRole('textbox').fill('research');
        await launchForm.getByLabel('Launch template').selectOption('docker');
        await launchForm.getByRole('button', { name: 'Create', exact: true }).click();
        await expect(dialog.getByRole('button', { name: 'Download research.jsonc' })).toBeVisible();

        await dialog.getByRole('button', { name: 'Actions for research.jsonc' }).click();
        await expect(page.getByRole('menuitem', { name: 'Preview Hub endpoints' })).toBeVisible();
        await page.screenshot({ path: testInfo.outputPath('configuration-file-menu.png'), animations: 'disabled' });
        await page.getByRole('menuitem', { name: 'Preview Hub endpoints' }).click();
        await expect(dialog.getByRole('listitem', { name: 'research.jsonc', exact: true }))
            .toContainText('ws://172.17.0.1:');
        await expect(dialog.getByRole('listitem', { name: 'local.jsonc', exact: true })).not.toContainText('ws://');
        await dialog.getByRole('button', { name: 'Close preview' }).click();

        await rowAction(page, 'research.yaml', 'Rename…');
        const rename = dialog.getByRole('form', { name: 'Rename research.yaml' });
        await expect(rename.getByRole('textbox')).toBeFocused();
        await expect(rename.getByRole('textbox')).toHaveValue('research');
        await page.keyboard.press('Escape');
        await expect(rename).toHaveCount(0);
        await expect(dialog.getByRole('button', { name: 'Actions for research.yaml' })).toBeFocused();

        await rowAction(page, 'research.yaml', 'Rename…');
        await rename.getByRole('textbox').fill('personal');
        const source = join(config.dataDir, 'configs', 'worker', 'research.yaml');
        await writeFile(source, '# External edit\n' + await readFile(source, 'utf8'));
        await rename.getByRole('textbox').press('Enter');
        await expect(dialog.getByRole('alert')).toContainText('changed');
        await rename.getByRole('button', { name: 'Cancel' }).click();
        await rowAction(page, 'research.yaml', 'Rename…');
        await rename.getByRole('textbox').fill('personal');
        await page.screenshot({ path: testInfo.outputPath('configuration-inline-rename.png'), animations: 'disabled' });
        await rename.getByRole('textbox').press('Enter');
        await expect(dialog.getByRole('button', { name: 'Download personal.yaml' })).toBeVisible();

        await expect(session).not.toHaveAttribute('open', '');
        await session.locator('summary').click();
        await expect(session.getByLabel('Launch configuration')).toHaveValue('local');
        await expect(session.getByLabel('Worker configuration')).toHaveValue('default');
        await session.getByLabel('Launch configuration').selectOption('research');
        await session.getByLabel('Worker configuration').selectOption('personal');
        page.once('dialog', message => message.accept());
        await session.getByRole('button', { name: 'Apply to review' }).click();
        await expect(dialog.getByRole('status')).toContainText('Configuration applied to review');
        expect(hub.registry.get('review')?.spec).toMatchObject({ launchConfig: 'research', workerConfig: 'personal' });

        page.once('dialog', message => message.dismiss());
        await rowAction(page, 'research.jsonc', 'Delete…');
        await expect(dialog.getByRole('button', { name: 'Download research.jsonc' })).toBeVisible();
        page.once('dialog', message => message.accept());
        await rowAction(page, 'research.jsonc', 'Delete…');
        await expect(dialog.getByRole('button', { name: 'Download research.jsonc' })).toHaveCount(0);
        expect(hub.registry.get('review')?.spec.launchConfig).toBe('research');
    } finally {
        await page.close();
        await hub.stop();
        await rm(config.dataDir, { recursive: true, force: true });
    }
});
