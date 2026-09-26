/**
 * @file browser test configuration.
 *
 * This replaces `test/helpers/browser.js`, a hand-written CDP client that
 * existed because the panel had no build step and no dependency budget. It
 * bought a real browser check for one file and nothing else: no screenshots, no
 * traces, no retries, and it skipped silently when the machine's Chrome was
 * missing or unloadable — which is exactly what happened on the machine this
 * was written on, where the cached Chrome cannot start for want of libnss3.
 *
 * Playwright brings its own browser, so "is a browser available" stops being a
 * property of the host and becomes a property of the install.
 *
 * Two servers, because the panel needs both halves:
 *
 * - `vite preview` serves the built bundle over the `preview.proxy` in
 *   `vite.config.ts`, which forwards `/api` and the `/panel/ws` upgrade to the
 *   hub. A static preview alone cannot do WebSockets at all, which is why the
 *   earlier version of this file could only ever load a shell.
 * - `test/browser/stub-hub.mjs` is a hub that speaks the panel protocol and can
 *   be told to emit, confirm and restart on demand. A real hub needs a real
 *   worker and a real model to produce those, and cannot be asked to pretend it
 *   restarted halfway through a test.
 */
import { defineConfig, devices } from '@playwright/test';

/** Port for the preview server; unlikely to collide with a running hub. */
const PREVIEW_PORT = 4173;

/** Port for the scripted hub the preview server proxies to. */
const STUB_PORT = Number(process.env.STUB_HUB_PORT ?? 4180);

export default defineConfig({
    testDir: './test/browser',
    testMatch: '*.spec.ts',
    // The stub hub is a single shared server that tests reset and script, so
    // they cannot run at the same time as each other. Each one is fast; making
    // them independent would mean one stub per worker, which is a lot of
    // machinery for the seconds it would save.
    fullyParallel: false,
    workers: 1,
    forbidOnly: Boolean(process.env.CI),
    retries: process.env.CI ? 1 : 0,
    reporter: process.env.CI ? 'github' : 'list',

    use: {
        baseURL: `http://127.0.0.1:${PREVIEW_PORT}`,
        // Artifacts only on failure: a passing run should leave nothing behind.
        trace: 'on-first-retry',
        screenshot: 'only-on-failure',
    },

    projects: [
        { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    ],

    webServer: [
        {
            command: `node test/browser/stub-hub.mjs`,
            url: `http://127.0.0.1:${STUB_PORT}/api/meta`,
            reuseExistingServer: !process.env.CI,
            timeout: 30_000,
            env: { STUB_HUB_PORT: String(STUB_PORT) },
        },
        {
            // The build runs as part of the server command so a stale `dist` can
            // never be what the browser loads.
            // Probe and server must use the same address. On some runners
            // localhost resolves to ::1, while the probe uses 127.0.0.1.
            command: `npm run build && npx vite preview --host 127.0.0.1 `
                + `--port ${PREVIEW_PORT} --strictPort`,
            url: `http://127.0.0.1:${PREVIEW_PORT}/`,
            reuseExistingServer: !process.env.CI,
            timeout: 120_000,
            stdout: 'pipe',
            stderr: 'pipe',
            env: { SIMPLEX_HUB_ORIGIN: `http://127.0.0.1:${STUB_PORT}` },
        },
    ],
});
