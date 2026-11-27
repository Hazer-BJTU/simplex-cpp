import { expect, test, type Page } from '@playwright/test';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { defaultConfig, hubRoot } from '../../src/config.ts';
import { createHub, type Hub } from '../../src/hub.ts';
import { createLogger } from '../../src/log.ts';
import type { ConfigFile } from '../../ui/index.ts';
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

/** Run the built panel against a private library and the Hub's actual revision checks. */
async function withConfigurationHub(page: Page, check: (hub: Hub, url: string) => Promise<void>) {
    const config = defaultConfig();
    config.dataDir = await mkdtemp(join(tmpdir(), 'simplex-file-list-'));
    config.listen.port = config.toolRequests.port = 0;
    const hub = createHub({ config, hubRoot, log: createLogger({ level: 'silent' }) });
    try {
        const { url } = await hub.start();
        await check(hub, url);
    } finally {
        await page.close();
        await hub.stop();
        await rm(config.dataDir, { recursive: true, force: true });
    }
}

test('row actions manage same-named files independently of the session selection', async ({ page }, testInfo) => {
    await withConfigurationHub(page, async (hub, url) => {
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
        const source = join(hub.config.dataDir, 'configs', 'worker', 'research.yaml');
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
    });
});

/** Take a real download at A, then save B through a separate API request. */
async function editAfterDownload(page: Page, url: string) {
    const path = url + '/api/configurations/worker/default';
    await page.goto(url);
    await page.getByRole('button', { name: 'Configurations', exact: true }).click();
    const transfer = page.waitForEvent('download');
    await page.getByRole('button', { name: 'Download default.yaml', exact: true }).click();
    const download = await transfer;
    const original: ConfigFile = await (await page.request.get(path)).json();
    const bytes = await readFile((await download.path())!, 'utf8');
    expect(bytes).toBe(original.text);
    const saved = await page.request.put(path, { data: {
        text: '# External update\n' + bytes, revision: original.revision,
    } });
    expect(saved.status()).toBe(200);
    const updated: ConfigFile = await saved.json();
    expect(updated.revision).not.toBe(original.revision);
    return { path, original, updated, upload: {
        name: download.suggestedFilename(), mimeType: 'text/plain', buffer: Buffer.from('# Local edit\n' + bytes),
    } };
}

test('renaming carries the downloaded revision to the new row', async ({ page }) => {
    await withConfigurationHub(page, async (hub, url) => {
        const { path, original, updated, upload } = await editAfterDownload(page, url);
        async function expectUploadConflict(id: string) {
            await rowAction(page, id + '.yaml', 'Replace from file…');
            const form = page.getByRole('form', { name: 'Replace ' + id + '.yaml' });
            await form.locator('input[type=file]').setInputFiles(upload);
            const attempted = page.waitForResponse(url + '/api/configurations/worker/' + id);
            await form.getByRole('button', { name: 'Replace', exact: true }).click();
            const response = await attempted;
            expect(response.status()).toBe(409);
            expect(response.request().postDataJSON().revision).toBe(original.revision);
            expect(await readFile(join(hub.config.dataDir, 'configs', 'worker', id + '.yaml'), 'utf8')).toBe(updated.text);
            await form.getByRole('button', { name: 'Cancel' }).click();
        }

        await expectUploadConflict('default');
        await rowAction(page, 'default.yaml', 'Rename…');
        const form = page.getByRole('form', { name: 'Rename default.yaml' });
        await form.getByRole('textbox').fill('renamed');
        const moved = page.waitForResponse(path + '/rename');
        await form.getByRole('button', { name: 'Rename', exact: true }).click();
        const response = await moved;
        expect(response.status()).toBe(200);
        expect(response.request().postDataJSON().revision).toBe(updated.revision);
        await expectUploadConflict('renamed');
    });
});

test('deleting with a stale download refuses until the operator downloads the new revision', async ({ page }) => {
    await withConfigurationHub(page, async (_hub, url) => {
        const { path, original, updated } = await editAfterDownload(page, url);
        async function confirmDelete() {
            const attempted = page.waitForResponse(r => r.url() === path && r.request().method() === 'DELETE');
            page.once('dialog', message => message.accept());
            await rowAction(page, 'default.yaml', 'Delete…');
            return attempted;
        }
        const stale = await confirmDelete();
        expect(stale.status()).toBe(409);
        expect(stale.request().postDataJSON().revision).toBe(original.revision);
        await expect(page.getByRole('alert')).toContainText('Download the current file before deleting it');
        const current = await page.request.get(path);
        expect(current.status()).toBe(200);
        expect((await current.json()).text).toBe(updated.text);

        const transfer = page.waitForEvent('download');
        await page.getByRole('button', { name: 'Download default.yaml', exact: true }).click();
        expect(await readFile((await (await transfer).path())!, 'utf8')).toBe(updated.text);
        const accepted = await confirmDelete();
        expect(accepted.status()).toBe(200);
        expect(accepted.request().postDataJSON().revision).toBe(updated.revision);
    });
});

test('both ends of a long launch menu are reachable by pointer in landscape', async ({ page }, testInfo) => {
    await page.setViewportSize({ width: 667, height: 375 });
    const launches = Array.from({ length: 16 }, (_, index) => 'launch-' + index);
    const id = launches[8]!;
    await page.route('**/api/configurations', route => route.fulfill({ json: {
        worker: ['default'], launch: launches, editorAvailable: true,
    } }));
    await page.route(`**/api/configurations/launch/${id}/source`, route => route.fulfill({ json: {
        id, kind: 'launch', revision: 'r1', text: '{}', path: id + '.jsonc',
    } }));
    await open(page);
    await page.getByRole('button', { name: 'Open the session list' }).click();
    await page.getByRole('button', { name: 'Configurations', exact: true }).click();
    const trigger = page.getByRole('button', { name: `Actions for ${id}.jsonc` });
    await trigger.evaluate(node => node.scrollIntoView({ block: 'center' }));
    await trigger.click();
    const menu = page.getByRole('menu');
    await expect(menu).toBeInViewport({ ratio: 1 });
    expect(await menu.evaluate(node => node.scrollHeight > node.clientHeight)).toBe(true);
    const first = menu.getByRole('menuitem', { name: 'Download', exact: true });
    await menu.hover();
    await page.mouse.wheel(0, -400);
    await expect(first).toBeInViewport({ ratio: 1 });
    const transfer = page.waitForEvent('download');
    await first.click();
    expect((await transfer).suggestedFilename()).toBe(id + '.jsonc');

    await trigger.click();
    await menu.hover();
    await page.mouse.wheel(0, 400);
    const last = menu.getByRole('menuitem', { name: 'Delete…' });
    await expect(last).toBeInViewport({ ratio: 1 });
    await page.screenshot({ path: testInfo.outputPath('launch-menu-landscape.png'), animations: 'disabled' });
    const confirmation = page.waitForEvent('dialog');
    page.once('dialog', message => message.dismiss());
    await last.click();
    expect((await confirmation).message()).toContain(id + '.jsonc');
});

test('delayed keyboard validation returns focus to the originating row menu', async ({ page }) => {
    await page.route('**/api/configurations', route => route.fulfill({ json: {
        worker: ['first', 'second'], launch: [],
    } }));
    let releaseRead!: () => void;
    const delay = new Promise<void>(resolve => { releaseRead = resolve; });
    await page.route('**/api/configurations/worker/first/source', async route => {
        await delay;
        await route.fulfill({ json: { id: 'first', kind: 'worker', revision: 'r1', text: 'a: 1' } });
    });
    await page.route('**/api/configurations/worker/validate', route => route.fulfill({ json: { valid: true } }));
    await open(page);
    await page.getByRole('button', { name: 'Configurations', exact: true }).click();
    const trigger = page.getByRole('button', { name: 'Actions for first.yaml' });
    try {
        await trigger.press('Enter');
        await page.getByRole('menuitem', { name: 'Validate' }).press('Enter');
        await expect(trigger).toBeDisabled();
        await expect(page.getByRole('menu')).toHaveCount(0);
    } finally {
        releaseRead();
    }
    await expect(page.getByRole('status')).toContainText('first.yaml is valid');
    await expect(trigger).toBeFocused();
    await page.keyboard.press('Tab');
    await expect(page.getByRole('button', { name: 'Download second.yaml' })).toBeFocused();
});
