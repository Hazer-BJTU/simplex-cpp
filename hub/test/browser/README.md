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
  Only the latest projection and its retained source-to-key map are strongly
  retained. Shared event/outbox evidence preserves an execution's DOM identity
  when trimming only its beginning; reused wire IDs without that evidence do
  not inherit another execution's key. Disclosure maps discard removed content.
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
replaying security decisions. Either link's loss also revokes an already granted
retry permission; socket-open notifications cannot restore it before a fresh
check finishes. Attempt IDs fence late rejection replies.

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

To repeat the baseline comparison, create a detached worktree at `abc05ee`,
copy the current `panel-performance.spec.ts` and `stub-hub.mjs` into its
`hub/test/browser/` directory, and install dependencies there. Run the same
commands in each worktree with separate output paths; stop each preview before
starting the other. The production application stays at the compared revision,
while both use exactly the same workload. The copied harness intentionally makes
the baseline report dirty. Keep reports outside a disposable worktree and remove
that worktree with `git worktree remove` after reviewing the artifacts.

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
`abc05ee` and optimized revision `8abd9f0`. The baseline differs only in the copied
benchmark/stub harness needed to run the identical newer workload. Environment:
Linux x86_64, Intel Core Ultra 9 275HX, Node 24.15.0, Chromium 153.0.8010.12,
1280 × 720, no CPU throttling. Each sample starts with 320 turns / 1,920 events,
120-line code blocks and 400-line tool outputs, then exercises eight approvals,
pane switches, an 81-event burst and 120 timed response events while typing.
Initial page hydration is outside the measured interval.

| Measure (range across three samples) | Baseline | Optimized |
| --- | ---: | ---: |
| Total round-projection CPU time | 7,425.6–7,970.6 ms | 540.6–613.7 ms |
| Markdown renders | 20,729–20,784 | 200 |
| Long tasks ≥50 ms | 198–215 | 0–9 |
| p95 animation-frame interval | 183.3–199.9 ms | 50.0–50.1 ms |
| p95 local pane handler-to-paint | 31.1–32.7 ms | 24.3–28.2 ms |
| p95 input handler-to-paint | 29.5–36.8 ms | 30.9–40.3 ms |
| End-of-sample DOM nodes | 5,817 | 5,770 |
| End-of-sample JS heap (decimal MB) | 64.0–76.6 | 31.2–56.8 |

The 200 optimized Markdown renders correspond to the 200 new response documents;
unchanged history is not reparsed. Local timing already met the reference target
before this change; the substantial improvement is reduced repeated work and
fewer long stalls. CPU profiling adds overhead, frame gaps include test-driving
work, and a heap snapshot cannot prove the absence of leaks. These numbers do
not guarantee latency on every device. The updated samples contain 0, 9 and 0
long tasks; the nine tasks are 50–62 ms. That sample's CPU profile also records
substantial Playwright-injected DOM traversal (`visitNode`/`visitChild`), so these
counts cannot be interpreted as application-only stalls. Compare distributions and stack evidence,
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

## Large completed-output measurements

The optional `panel-large-output.spec.ts` measures a completed response with a
small exact final answer and 128 KiB, 1 MiB, 3 MiB or 8 MiB of reasoning. It
reports render time, composer input, expansion, observed long tasks, DOM nodes,
coarse browser heap estimates and profiling counters. Timings are observations,
not CI gates. Normal CI runs the literal-reasoning/page correctness tests.

Generate offline cases, then run the production panel:

```bash
node test/browser/large-output-cases.mjs /tmp/simplex-output-cases
SIMPLEX_LARGE_OUTPUT_BENCHMARK=1 \
SIMPLEX_OUTPUT_CASES=/tmp/simplex-output-cases/after.json \
SIMPLEX_OUTPUT_REPORT=/tmp/simplex-output-cases/report.json \
npx playwright test panel-large-output.spec.ts
```

For a baseline checkout, copy the benchmark spec into its browser test directory
and pass `before.json`. Use that checkout's production build and stop any preview
server from the other checkout before switching. `envelopeBytes` deliberately
measures the content-bearing wrapper (`data` and the old duplicate `raw.data`),
not a complete authenticated WebSocket frame. The real-Hub integration test
measures complete messages and verifies repeated replay remains connected.

Reference run on 2026-10-08: baseline `dd37792` (v0.4.1), WSL2 Linux 6.18.33.2,
Intel Core Ultra 9 275HX / 12 exposed CPUs, Node 24.15.0, Playwright Chromium
153.0.8010.12, 1280×720 viewport, no CPU throttling. Native test compilation was
running independently during the reference sample; do not interpret small timing
differences as portable performance guarantees.

| Reasoning | Before render | After render | Before longest task | After longest task | Before / after content wrapper |
| --- | ---: | ---: | ---: | ---: | ---: |
| 128 KiB | 83 ms | 26 ms | 60 ms | none observed | 262,521 / 4,313 bytes |
| 1 MiB | 351 ms | 34 ms | 249 ms | none observed | 2,097,529 / 4,314 bytes |
| 3 MiB | 994 ms | 10 ms | 605 ms | none observed | 6,291,833 / 4,314 bytes |
| 8 MiB | 2,521 ms | 15 ms | 1,415 ms | none observed | 16,777,593 / 4,314 bytes |

Composer fill/verification remained 8–17 ms in this fixture. Expansion includes
Playwright actionability and the explicit 50 ms observation window: before
279–1,493 ms, after 304–310 ms. The unchanged final answer accounted for one
Markdown render; baseline reasoning added another, while the new reasoning
component added **zero**, expanded or collapsed. Browser-reported heap estimates
were coarse (19.3 MB before / 10 MB after), not a worker-RSS or canonical-state
measurement. State-copy/persistence benchmarking remains separate (#65).

The tests also cover literal Markdown/HTML-like reasoning, multipart Unicode
answer pages, live/replay/history identity, original snapshot values, subsequent
provider replay, authorized parent retrieval and source expiration. Large answers
use a 512 KiB encoded preview or explicit pages; this report's tiny-answer fixture
does not claim that every large answer can render in one frame.
