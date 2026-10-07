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

## Interaction performance

The panel keeps its existing layout and controls. Performance work separates
immediate protocol ingestion from visual publication:

- The store still folds every event in order. Confirmation, connection,
  admission and terminal updates bypass transcript coalescing. Ordinary output
  publishes the latest snapshot once per animation frame or an 8 ms timer,
  whichever runs first; hidden-tab timer throttling can delay painting, but
  never delays ingestion or creates an unbounded event queue.
- A hidden Conversation pane unsubscribes from visual updates while retaining
  its DOM, disclosures and scroll position. Returning reads the current store.
  Hidden Plan Markdown likewise waits until the pane becomes visible.
- Round projection always executes the reference full fold. Weak caches reuse
  immutable parsing, and structural sharing retains unchanged rendering records.
  Only the latest projection is strongly retained. Stable execution keys survive
  retained-tail trimming; disclosure maps discard keys for removed content.
- Markdown, completed rounds and tool cards skip unchanged rendering. Large
  argument/output bodies mount on expansion. A shared time formatter avoids
  creating an `Intl.DateTimeFormat` for every historical event on every update.
- One scroll owner coalesces layout reads/writes after commits and resizes.
  Reader input controls following; content growth cannot turn it back on. A
  visible anchor retains the reader's offset, including across pane switches
  and approval-region resizing, without treating entrance transforms as growth.

Approval submission state belongs to the transport supervisor, scoped by
session, worker, run and prompt creation. Closing a dialog does not unlock a
pending decision. Submission changes the existing button's contents without
inserting a waiting paragraph or optimistically removing the prompt. After
8 seconds without settlement, an independently bounded snapshot request checks
whether the prompt remains open; failure keeps the outcome unknown. **Check
outcome** or **Review** can check again without resending a decision. Only an
explicit action retries after authoritative reconciliation. Panel and worker
links are tracked independently; reconnect checks unknown outcomes without
replaying security decisions. Attempt IDs fence late rejection replies.

### Reproducible reports

Use a production bundle, not the development server. Run other test suites and
benchmarks separately so their processes do not distort CPU/frame measurements:

```sh
# From hub/: three matched near-capacity samples, no credentials or live LLM.
PANEL_BENCHMARK=1 npx playwright test panel-performance.spec.ts \
  --repeat-each=3 --output=performance-results/retained-tail

# Other fixtures; keep the same browser and host for before/after comparisons.
PANEL_BENCHMARK=1 PANEL_FIXTURE_TURNS=8 \
  npx playwright test panel-performance.spec.ts --output=performance-results/short
PANEL_BENCHMARK=1 PANEL_FIXTURE_TURNS=8 PANEL_HISTORY_TURNS=200 \
  npx playwright test panel-performance.spec.ts --output=performance-results/restored
PANEL_BENCHMARK=1 PANEL_VIEWPORT_WIDTH=360 \
  npx playwright test panel-performance.spec.ts --output=performance-results/narrow
```

The opt-in **Panel performance report** workflow runs all four fixtures, three
samples each. Its artifacts retain metrics, Chrome CPU profiles, Playwright
traces, recordings, and the matching production bundle/source maps for 14 days.
It is manual only; environment-sensitive timings are not PR/merge/release gates.
Open a trace with `npx playwright show-trace <trace.zip>`, and import
`panel.cpuprofile` into Chrome DevTools Performance. Profiles include browser
and Playwright selector work: attribute a cost to application code only after
checking its stack and the matching source maps.

Adding `?panel_profile=1` exposes fixed, content-free counters through
`window.__simplexPanelProfile.read()` and `.reset()`. Normal panels do not sample
these clocks or keep profiling data. Reports also record commit/dirty status,
fixture dimensions, browser, Node, CPU, viewport, long tasks, frame intervals,
DOM count, available heap size, and Event Timing entries above the browser's
16 ms threshold. Event Timing is supplementary, not an exhaustive interaction
log. Pane/input timing measures handler-to-two-animation-frames; input timing
excludes pre-dispatch queueing, which Event Timing reports separately. Two frames
are a repeatable paint approximation, not a browser INP score.

### Reference comparison

On 2026-10-07, three samples of the same fixture were compared at baseline
`abc05ee` and optimized revision `06fc9c4`. The baseline differs only in the copied
benchmark/stub harness needed to run the identical newer workload. Environment:
Linux x86_64, Intel Core Ultra 9 275HX, Node 24.15.0, Chromium 153.0.8010.12,
1280 × 720, no CPU throttling. Each sample starts with 320 turns / 1,920 events,
120-line code blocks and 400-line tool outputs, then exercises eight approvals,
pane switches, an 81-event burst and 120 timed response events while typing.
Initial page hydration is outside the measured interval.

| Measure (range across three samples) | Baseline | Optimized |
| --- | ---: | ---: |
| Total round-projection CPU time | 7,425.6–7,970.6 ms | 426.1–447.3 ms |
| Markdown renders | 20,729–20,784 | 200 |
| Long tasks ≥50 ms | 198–215 | 0–2 |
| p95 animation-frame interval | 183.3–199.9 ms | 50.0 ms |
| p95 local pane handler-to-paint | 31.1–32.7 ms | 27.1–31.4 ms |
| p95 input handler-to-paint | 29.5–36.8 ms | 29.0–30.3 ms |
| End-of-sample DOM nodes | 5,817 | 5,770 |
| End-of-sample JS heap (decimal MB) | 64.0–76.6 | 44.7–53.5 |

The 200 optimized Markdown renders correspond to the 200 new response documents;
unchanged history is not reparsed. Local timing already met the reference target
before this change; the substantial improvement is reduced repeated work and
fewer long stalls. CPU profiling adds overhead, frame gaps include test-driving
work, and a heap snapshot cannot prove the absence of leaks. These numbers do
not guarantee latency on every device. Compare distributions and stack evidence,
not one best run; retain recordings to assess continuity separately.

### Acceptance checklist

Run `panel-responsiveness.spec.ts` alongside the existing browser suites. Check
short and near-capacity conversations, restored history, large details, pending
asks in other sessions/headless workers, delayed settlement, and replay. Verify:

1. Typing/IME and Conversation ↔ Plan remain responsive while output arrives;
   returning shows current output and preserves disclosures and scroll intent.
2. Existing spinner elements stay mounted. Readers above the bottom keep their
   anchor within 2 px after growth and approval resizing; Jump to latest resumes
   following. Reduced motion removes animation without changing behavior.
3. Double/opposite decisions send once; Later/Review preserves the lock. Reject,
   lost reply, failed outcome check, expiry, Panel disconnect and Worker-only
   disconnect cannot settle another prompt or silently resend a decision.
4. Terminal/cancellation/approval state remains current during output bursts,
   including when animation frames are unavailable. Event order, history
   correlation, compact/continue and reconnect remain covered by existing gates.
5. Narrow/short viewports, both themes, zoom and classic scrollbar gutters retain
   the accepted appearance. Review mobile keyboard behavior on a real device;
   desktop automation is not a substitute for it.

For the actual transport and worker rather than the stub, run `npm run test:e2e`
with `SIMPLEX_WORKER_BIN` pointing to a current worker executable and
`SIMPLEX_E2E_REQUIRED=1`. Then start a scratch Hub with `--mock` and that executable,
and run `SIMPLEX_HUB_ORIGIN=http://127.0.0.1:<port> npm run check:panel` against it.
This checks an actual ask → tool execution → final response → page reload without
live model credentials. Stop the scratch Hub before running E2E: both may use
the default remote-tool endpoint. It does not validate external provider latency
or replace a manual real-model session.
