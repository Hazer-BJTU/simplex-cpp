/**
 * @file the acceptance check for the panel: a real hub, a real worker, a browser.
 *
 * `panel-shell.spec.ts` runs in CI against `stub-hub.mjs`, which is fast and
 * scriptable but is not the hub. This is the other half: a real hub serving the
 * panel it will actually ship, running the real `simplex_worker` against the
 * offline mock provider — create a session, start it, send a message, answer
 * the tool approval, and read the transcript the worker produced.
 *
 * The browser talks to the hub's own origin, with no build server in between.
 * That is the point of doing it this way: the panel is loaded from `web/dist`
 * over the hub's static file server, over the hub's socket, with the hub's own
 * `Origin`/`Host` check in the path. A preview server would test the bundle and
 * skip the deployment.
 *
 * It is a script rather than a Playwright test because it needs the C++ build
 * and a running hub, and a test that skips itself when the build is missing is
 * a test nobody runs. Run it deliberately:
 *
 * ```sh
 * npm run build
 * node bin/simplex-hub.ts --mock --listen 127.0.0.1:8899 --data-dir /tmp/p8-hub &
 * npm run check:panel
 * ```
 *
 * Everything it prints is evidence: the transcript as rendered, the panel's own
 * counters against the hub's, and any console error. The hub origin and
 * screenshot directory come from the environment, so it can be pointed
 * somewhere else.
 */
import { chromium } from '@playwright/test';

const HUB = process.env.SIMPLEX_HUB_ORIGIN ?? 'http://127.0.0.1:8899';
/** The panel is the hub's own front page; override only to check a proxy. */
const PANEL = process.env.SIMPLEX_PANEL_URL ?? `${HUB}/`;
const SESSION = `p8-${Date.now().toString(36)}`;

/** Screenshots land here; override to keep a record of a particular run. */
const SHOTS = process.env.SIMPLEX_SHOT_DIR ?? '/tmp';

const post = (path, body) => fetch(`${HUB}${path}`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify(body ?? {}),
});
const get = (path) => fetch(`${HUB}${path}`).then((response) => response.json());

// The panel's create form takes an id only; provider and model are hub
// configuration, so the spec is supplied here the way hub.config.jsonc would.
const created = await post('/api/sessions', {
    session: SESSION,
    spec: { provider: 'mock', model: 'mock-auto' },
});
if (!created.ok) throw new Error(`could not create ${SESSION}: ${created.status}`);
console.log(`created ${SESSION}`);

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
const errors = [];
page.on('pageerror', (error) => errors.push(`pageerror: ${error.message}`));
page.on('console', (message) => {
    if (message.type() === 'error') errors.push(`console: ${message.text()}`);
});

await page.goto(PANEL);
await page.getByText('connected', { exact: true }).waitFor({ timeout: 10_000 });
await page.getByTestId('session-row').filter({ hasText: SESSION }).click();

await page.getByRole('button', { name: 'Start' }).click();
// Scoped to this session's row: another session being attached says nothing
// about this one.
await page.getByTestId('session-row').filter({ hasText: SESSION })
    .getByText('worker attached').waitFor({ timeout: 20_000 });
console.log('worker attached');

await page.getByLabel('message').fill('Run the mock command, please.');
await page.getByRole('button', { name: 'Send' }).click();

// The mock provider's scripted first turn proposes a call that needs approval,
// so this exercises the approval path end to end rather than only asserting
// that a prompt can be drawn.
const banner = page.getByTestId('approval-banner');
await banner.waitFor({ timeout: 60_000 });
console.log(`approval banner: ${(await banner.innerText()).split('\n').slice(0, 2).join(' | ')}`);

// The dialog opens itself, and nothing in it is armed: a decision has to be
// made rather than pressed Enter into.
const dialog = page.getByRole('dialog');
await dialog.waitFor({ timeout: 10_000 });
console.log(`approval dialog: ${(await dialog.innerText()).split('\n').slice(0, 3).join(' | ')}`);
await page.screenshot({ path: `${SHOTS}/p8-approval.png` });

await dialog.getByRole('button', { name: 'Approve' }).click();
await page.getByTestId('approval-banner').waitFor({ state: 'detached', timeout: 30_000 });
console.log('approval settled');

await page.getByTestId('tool-card').filter({ hasText: 'run_command' }).last()
    .waitFor({ timeout: 30_000 });
await page.waitForTimeout(3000);

console.log('--- transcript as rendered ---');
console.log(await page.getByTestId('transcript').innerText());
console.log('--- panel counters ---');
console.log(await page.getByTestId('transcript-stats').innerText());
await page.screenshot({ path: `${SHOTS}/p8-real-hub.png` });

// A reload is the harshest test of the replay path: a fresh page, an empty
// store, and a cursor of zero against a hub that already has history.
await page.reload();
await page.getByTestId('tool-card').first().waitFor({ timeout: 20_000 });
console.log('--- after a reload ---');
console.log(await page.getByTestId('transcript-stats').innerText());

const state = await get(`/api/sessions/${SESSION}`);
console.log('--- hub-side counters ---');
console.log(JSON.stringify({
    events: state.session.stats.events,
    gaps: state.session.stats.gaps,
    duplicates: state.session.stats.duplicates,
    protocolErrors: state.session.stats.protocolErrors,
    last_event: state.session.last_event,
    open_confirmations: state.session.confirmations.length,
}));

console.log('--- console errors ---');
console.log(errors.length ? errors.join('\n') : '(none)');
await browser.close();
