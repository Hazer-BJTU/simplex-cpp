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
 * The dev server under test is `vite preview` over a fresh build, not the hub:
 * the panel is not served by the hub until the rewrite replaces the old one,
 * and a test that needed the hub would be testing the wrong thing anyway.
 */
import { defineConfig, devices } from '@playwright/test';

/** Port for the preview server; unlikely to collide with a running hub. */
const PREVIEW_PORT = 4173;

export default defineConfig({
    testDir: './test/browser',
    fullyParallel: true,
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

    webServer: {
        // The build runs as part of the server command so a stale `dist` can
        // never be what the browser loads.
        command: `npm run build && npx vite preview --port ${PREVIEW_PORT} --strictPort`,
        url: `http://127.0.0.1:${PREVIEW_PORT}/app.html`,
        reuseExistingServer: !process.env.CI,
        timeout: 120_000,
        stdout: 'ignore',
        stderr: 'pipe',
    },
});
