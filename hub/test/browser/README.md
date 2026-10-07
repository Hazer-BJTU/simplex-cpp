# Panel browser checks

Run from `hub/` after `npm ci` and `npx playwright install chromium`:

```sh
npm run typecheck
npx playwright test
```

Playwright starts its own scripted Hub and a freshly built Vite preview. Stop
manually started preview/stub servers first to avoid testing a stale bundle or
stub. If an HTTP proxy intercepts loopback, exclude `127.0.0.1`/`localhost` from it.

`panel-reading-surface.spec.ts` captures the same conversation, cancellation,
failure and long approval at 360, 768 and 1440 pixels in both themes. Each case
saves PNGs and a short WebM interaction recording in `test-results/`. The CI
`playwright-report` artifact retains these on success as well as failure.

For a visual comparison, serve the base revision's panel bundle with
`SIMPLEX_HUB_ORIGIN=http://127.0.0.1:4180` on port 4173, run the current preview
fixtures, then stop that server and rerun against the current build:

```sh
npx playwright test panel-reading-surface.spec.ts \
  --grep 'reading surface preview' --output=/tmp/panel-before
# Stop the baseline preview; Playwright builds the current checkout next.
npx playwright test panel-reading-surface.spec.ts --output=/tmp/panel-after
```

Use the same browser, theme and viewport when comparing images. The behavioral
checks are separate from visual review: they cover shared alignment, bounded
input growth, mode/selection continuity, IME, stable cancellation, narrow/short
viewports and stale approval actions. Existing suites also cover keyboard focus,
contrast, reduced motion, reconnection, scroll preservation, Plan updates,
configuration editing and headless restrictions.

Keep `auto-compact-history.test.js` and the real-worker E2E tests as gates when
changing transcript presentation. The hidden `round.protocol` data still owns
execution identities and response placement; removing an inline timeline must
not remove that data. A passing suite does not imply visual acceptance: review
screenshots and recordings separately, including browser zoom and a real mobile
software keyboard. A shortened desktop viewport approximates keyboard space,
but does not validate a device's IME or visual-viewport behavior.
