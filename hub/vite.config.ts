/**
 * @file panel build configuration.
 *
 * The panel is a separate build from the hub: `tsc` type-checks the server and
 * never emits, while Vite bundles the browser half into `web/dist`. The static
 * file server reads that directory, so the two concerns meet in one place and
 * nowhere else.
 *
 * `web/index.html` is the entry, and Vite's default input picks it up: the
 * panel is one page, so there is nothing to name explicitly. The old
 * build-free panel that `app.html` existed to sit beside is gone, which is why
 * the explicit input went with it.
 */
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

const here = dirname(fileURLToPath(import.meta.url));

/** The hub the dev server and the preview server proxy to. */
const HUB_ORIGIN = process.env.SIMPLEX_HUB_ORIGIN ?? 'http://127.0.0.1:8800';

/**
 * The paths that belong to the hub rather than to the panel bundle.
 *
 * Shared by `server` and `preview` because they must not disagree: a browser
 * test that passes against the dev server and fails against the built bundle
 * is a test of the wrong thing. `preview` had no proxy at all until the panel
 * needed one, which is why the browser tests could only ever load a shell.
 *
 * `changeOrigin` is deliberately **not** set. The hub refuses a panel upgrade
 * whose `Origin` does not match its `Host` — that is its defence against a page
 * the operator visits driving a hub on loopback — and rewriting `Host` to the
 * target would make every proxied upgrade look exactly like that attack. The
 * proxy therefore forwards the browser's own `Host`, which is what the hub sees
 * when it serves the panel itself.
 */
const hubProxy = {
    '/api': { target: HUB_ORIGIN },
    '/panel/ws': { target: HUB_ORIGIN, ws: true },
};

export default defineConfig({
    root: resolve(here, 'web'),
    plugins: [react(), tailwindcss()],
    build: {
        outDir: resolve(here, 'web', 'dist'),
        emptyOutDir: true,
        // The panel is served from the hub's own origin, so asset URLs must be
        // absolute rather than relative to the entry document.
        sourcemap: true,
    },
    server: {
        // The dev server exists so the panel can be iterated with hot reload
        // against a real hub, rather than by rebuilding and refreshing.
        port: 5273,
        proxy: hubProxy,
    },
    preview: {
        // The browser tests run against the built bundle through this proxy, so
        // they exercise what the hub will actually serve — including the
        // WebSocket upgrade, which a static preview alone cannot provide.
        port: 4173,
        proxy: hubProxy,
    },
});
