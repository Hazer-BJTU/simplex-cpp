/**
 * @file panel build configuration.
 *
 * The panel is a separate build from the hub: `tsc` type-checks the server and
 * never emits, while Vite bundles the browser half into `web/dist`. The static
 * file server reads that directory, so the two concerns meet in one place and
 * nowhere else.
 *
 * `app.html` is the entry today rather than `index.html`, because
 * `web/index.html` is still the panel being replaced. The old file and the new
 * app coexist until the new one is complete, which keeps the hub usable
 * throughout the rewrite instead of trading a working panel for a half-built
 * one. The rename happens when the transcript lands.
 */
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

const here = dirname(fileURLToPath(import.meta.url));

/** The hub the dev server proxies to; `npm start` in another terminal. */
const HUB_ORIGIN = process.env.SIMPLEX_HUB_ORIGIN ?? 'http://127.0.0.1:8800';

export default defineConfig({
    root: resolve(here, 'web'),
    plugins: [react(), tailwindcss()],
    build: {
        outDir: resolve(here, 'web', 'dist'),
        emptyOutDir: true,
        // Without this Vite would treat `web/index.html` — still the panel being
        // replaced — as the entry, and the build would quietly bundle the old
        // panel into `dist`. Naming the entry explicitly is what keeps "which
        // panel did that build?" from being a question.
        rollupOptions: {
            input: resolve(here, 'web', 'app.html'),
        },
        // The panel is served from the hub's own origin, so asset URLs must be
        // absolute rather than relative to the entry document.
        sourcemap: true,
    },
    server: {
        // The dev server exists so the panel can be iterated with hot reload
        // against a real hub, rather than by rebuilding and refreshing.
        port: 5273,
        proxy: {
            '/api': { target: HUB_ORIGIN, changeOrigin: true },
            '/panel/ws': { target: HUB_ORIGIN, ws: true, changeOrigin: true },
        },
    },
});
