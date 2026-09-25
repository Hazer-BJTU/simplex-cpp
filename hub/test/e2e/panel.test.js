/**
 * @file the panel, in a real browser, against a real worker.
 *
 * Every other test in this suite uses the panel protocol directly, so the panel
 * itself — 4k lines of HTML, CSS and modules with no build step — is only ever
 * exercised by a person clicking. This file closes that gap: it loads the panel
 * in headless Chrome, drives a genuine run through it, and reads the result out
 * of the DOM.
 *
 * What it proves: the modules load, the panel socket connects, a session is
 * selected, live events turn into rendered cards, a confirmation shows up as a
 * modal, clicking Approve really answers the worker, the run completes, and the
 * theme toggle switches token sets — with no uncaught exception along the way.
 *
 * What it does not prove: visual layout, every event renderer, keyboard
 * behaviour, or any browser other than the one it found. It is a smoke test
 * with a wide blast radius, not a UI suite.
 *
 * The browser is whatever the machine has (CHROME_BIN, then PATH, then a
 * puppeteer cache). CI sets PANEL_REQUIRE_BROWSER=1 so a missing browser is a
 * failure instead of a quiet skip.
 */
import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { browserRequired, connectPage, findBrowser, launchBrowser } from '../helpers/browser.js';
import { e2eSkip, startE2eHub } from '../helpers/e2e.js';
import { connectWorker, until } from '../helpers/worker.js';

/** Session used by this file; also the `?session=` deep link. */
const SESSION = 'e2e-panel';

/** Poll a page expression until it is truthy. */
async function waitForPage(page, expression, { timeout = 30000, label = expression } = {}) {
    try {
        return await until(() => page.evaluate(expression), { timeout, label });
    } catch (error) {
        const diagnostics = await page.evaluate(`JSON.stringify({
            badge: document.querySelector('#panel-badge')?.textContent ?? null,
            title: document.querySelector('#session-title')?.textContent ?? null,
            cards: document.querySelectorAll('#timeline article.card').length,
            modals: document.querySelectorAll('#modal-stack .modal').length,
            timeline: (document.querySelector('#timeline')?.textContent ?? '').slice(0, 400),
        })`).catch(() => 'unavailable');
        throw new Error(`${error.message}\npage state: ${diagnostics}\n`
            + `exceptions: ${JSON.stringify(page.exceptions)}`);
    }
}

describe('panel in a browser', { skip: e2eSkip }, () => {
    it('loads, follows a real run, answers a confirmation, and switches theme',
        { timeout: 180000 }, async () => {
            const browserPath = findBrowser();
            const canDrive = typeof WebSocket !== 'undefined';
            if (!browserPath || !canDrive) {
                const message = browserPath
                    ? 'this Node has no global WebSocket; the CDP client needs Node 22 or newer'
                    : 'no Chrome/Chromium found (set CHROME_BIN to point at one)';
                if (browserRequired()) assert.fail(message);
                console.error(`panel browser check skipped: ${message}`);
                return;
            }

            const ctx = await startE2eHub();
            const panel = await connectWorker(`${ctx.wsBase}/panel/ws`);
            let browser = null;
            let page = null;
            try {
                const created = await fetch(`${ctx.base}/api/sessions`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ session: SESSION, spec: { provider: 'mock', model: 'mock-auto' } }),
                });
                assert.equal(created.status, 201);

                browser = await launchBrowser({ executable: browserPath });
                page = await connectPage(browser.port);
                await page.send('Page.enable');
                await page.send('Runtime.enable');
                await page.send('Page.navigate', { url: `${ctx.base}/?session=${SESSION}` });

                // The panel socket, `welcome`, and the deep link all have to work
                // before anything else can: this is the wiring check.
                await waitForPage(page,
                    `document.querySelector('#panel-badge')?.textContent?.includes('connected') === true`,
                    { label: 'the panel socket to connect' });
                await waitForPage(page,
                    `document.querySelector('#session-title')?.textContent === '${SESSION}'`,
                    { label: 'the session to be selected' });

                // Start the worker through the REST API; the browser must notice
                // without a reload.
                const started = await fetch(`${ctx.base}/api/sessions/${SESSION}/start`, { method: 'POST' });
                assert.equal(started.status, 200);
                const session = ctx.hub.registry.get(SESSION);
                await until(() => session.connected && session.identity.state === 'live',
                    { timeout: 60000, label: 'the worker to connect' });

                // Drive the run from the panel protocol so the browser only has
                // to render what arrives.
                panel.send({ v: 1, type: 'subscribe', session: SESSION });
                await panel.waitFor((message) => message.type === 'subscribed', { timeout: 10000 });
                panel.send({
                    v: 1,
                    type: 'input',
                    session: SESSION,
                    content: [{ type: 'text', raw: 'Run the fixture command' }],
                });
                await panel.waitFor((message) => message.type === 'accepted', { timeout: 10000 });

                // `run_command` needs confirmation, so a modal must appear in the
                // browser, not just a message on the socket.
                await waitForPage(page,
                    `document.querySelectorAll('#modal-stack .modal').length >= 1`,
                    { timeout: 60000, label: 'the confirmation modal' });
                await waitForPage(page,
                    `[...document.querySelectorAll('#modal-stack button')]`
                    + `.some((button) => button.textContent === 'Approve' && !button.disabled)`,
                    { timeout: 60000, label: 'an enabled Approve button' });

                // Click it: this is the one path no protocol-level test covers.
                const approved = await page.evaluate(
                    `[...document.querySelectorAll('#modal-stack button')]`
                    + `.find((button) => button.textContent === 'Approve')?.click() ?? false`);
                void approved;

                await waitForPage(page,
                    `document.querySelectorAll('#modal-stack .modal').length === 0`,
                    { timeout: 60000, label: 'the modal to close after deciding' });
                await waitForPage(page,
                    `(document.querySelector('#timeline')?.textContent ?? '')`
                    + `.includes('finished after 1 tool result(s)')`,
                    { timeout: 90000, label: 'the run to render its final answer' });

                const rendered = await page.evaluate(`JSON.stringify({
                    cards: document.querySelectorAll('#timeline article.card').length,
                    groups: document.querySelectorAll('#timeline section.run-group').length,
                    stdout: (document.querySelector('#timeline')?.textContent ?? '').includes('mock stdout'),
                    theme: document.documentElement.dataset.theme,
                })`);
                const state = JSON.parse(rendered);
                assert.ok(state.cards >= 3, `expected several rendered cards, saw ${rendered}`);
                assert.ok(state.groups >= 1, `expected a run group, saw ${rendered}`);
                assert.ok(state.stdout, `tool output was not rendered: ${rendered}`);

                // Both token sets have to be reachable, not just declared.
                const before = await page.evaluate('document.documentElement.dataset.theme');
                await page.evaluate("document.querySelector('#theme-toggle').click()");
                const after = await page.evaluate('document.documentElement.dataset.theme');
                assert.notEqual(after, before, 'the theme toggle changed nothing');
                assert.ok(['light', 'dark'].includes(after), `unexpected theme: ${after}`);

                assert.deepEqual(page.exceptions, [], 'the panel raised an uncaught exception');
            } finally {
                page?.close();
                await browser?.close();
                await panel.close();
                await ctx.hub.stop();
            }
        });
});
