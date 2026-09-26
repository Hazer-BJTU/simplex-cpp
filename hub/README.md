# simplex hub

A Node.js server that runs `simplex_worker` sessions and gives them a browser
panel.

`simplex_shell` (in `core/example`) is a one-to-one terminal server: one
operator, one session, approvals typed as UUIDs. The hub is the multi-session
counterpart — it launches workers, speaks the worker protocol on their behalf,
collects their events, turns tool confirmations into buttons, and keeps the
process output around when something goes wrong.

The hub is an additive component. It changes no C++ code and no client
behaviour: it implements the worker side of
[`core/docs/worker-protocol.md`](../core/docs/worker-protocol.md) as written.

## Requirements

- Node.js 22.18 or newer (developed on 24). The floor is a functional
  requirement rather than a conservative one: the hub loads
  [`shared/protocol.ts`](shared/protocol.ts) directly through Node's TypeScript
  type stripping, which became the default in 22.18. Nothing is compiled on the
  server side.
- A built worker binary — `build/bin/simplex_worker` plus its `plugins/` and
  `prompts/` directories. Build it with the repository's normal CMake flow.
- One runtime dependency: [`ws`](https://github.com/websockets/ws). Everything
  else in `package.json` is a development dependency.

## Quick start

```sh
cd hub
npm install
npm run build        # bundles the panel into web/dist
npm start
```

Then open <http://127.0.0.1:8800>. Create a session, choose a provider profile,
and press *Start worker*.

For a real model, export the key the generated configuration refers to — the
hub writes `${DEEPSEEK_API_KEY}` verbatim and the worker expands it, so the
secret never passes through the hub:

```sh
export DEEPSEEK_API_KEY=sk-...
```

### In a disposable container

This is the way to drive the hub by hand. A session can propose arbitrary
commands through the process tools and, once confirmed, they run wherever the
worker runs — so the container is there to be the thing that gets deleted, not
your working directory:

```sh
docker build -f docker/Dockerfile.hub-test -t simplex-hub-test .
docker run --rm --init -p 127.0.0.1:8800:8800 simplex-hub-test
# → http://127.0.0.1:8800/?token=simplex-hub-dev
```

It carries the worker (Debug, built in the image), the hub, its dependencies,
and the offline mock provider, so it needs no key and no configuration. Add
`-e DEEPSEEK_API_KEY=sk-...` to use a real provider as well — a session chooses
its profile either way — and `-v simplex-hub-data:/data` to keep sessions after
the container is gone. `docker run -it --rm simplex-hub-test bash` gives a shell
in the same tree.

The image mounts nothing from the host, runs unprivileged, and publishes the port
to loopback only. See
[docker/README.md](../docker/README.md#the-hub-test-image) for what that does and
does not protect against.

### Try it without credentials


The hub can serve its own scripted model, which makes the whole chain —
including tool calls, confirmations, and persistence — clickable offline:

```sh
npm start -- --mock
```

Create a session with the `mock` provider profile (model `mock-auto`), start it,
send any message, and approve the `run_command` confirmation the mock proposes.
The mock scenarios are selected by model name:

| Model | Behaviour |
| --- | --- |
| `mock-flash`, `mock-auto`, `mock-tool` | propose a `run_command`, then finish once results arrive |
| `mock-text` | reply with text |
| `mock-echo` | echo the last user message |
| `mock-slow` | wait before replying, for cancellation testing |
| `mock-error` | fail the request |

## Workers in a container

The hub can run on the host and put every worker in a container, with the worker
connecting back over the Docker bridge. Nothing about the protocol changes: the
worker dials the hub, so the only thing that has to be right is *which address it
is told to dial*.

`hub.config.docker-worker.jsonc` is a working example. The short version:

```sh
npm run build
docker build -f docker/Dockerfile.hub-test -t simplex-hub-test .   # once, ~10 min
node bin/simplex-hub.ts -c hub.config.docker-worker.jsonc --mock \
    --listen 0.0.0.0:8800 --panel-token dev --data-dir /tmp/docker-hub
# then: http://127.0.0.1:8800/?token=dev
```

Three things make it work, and each is a way to get it wrong:

1. **`worker.connectHost`.** The hub writes its own address into every worker's
   configuration. With `--listen 0.0.0.0` that address would be `0.0.0.0` —
   which a worker reads as "myself" — so the hub substitutes loopback for a
   wildcard bind, and loopback is exactly what a container cannot use. Setting
   `connectHost` to the bridge address (`172.17.0.1`) is what replaces it. The
   mock provider is advertised at the same address, because the worker is what
   connects to it.
2. **`launcher.kind: "command"`.** The hub's extension point for "start a worker
   some other way". The template is expanded per session, so `docker run` gets
   the generated config path, the session id and the data directory without the
   hub knowing anything about Docker.
3. **The mounts.** The generated `config.yaml`, the session's snapshot and the
   captured log are all named by *absolute host paths* — so the data directory
   is mounted at the same path inside the container, and `promptsDir` is mounted
   the same way rather than pointed at the image's own copy. Overriding
   `promptsDir` to the image's path looks tidier and does not work: the mount
   would then name a host directory that does not exist, and Docker helpfully
   creates an empty one.

`--user {uid}:{gid}` is in the template for a reason that only shows up
afterwards: an image runs as root, so the worker writes `state.json` and
`session.lock` into the mounted data directory as root, and the operator who
owns that directory cannot delete them. The two placeholders are the invoking
user; on a platform that has no uid to report, a template that asks for one is
refused at spawn time rather than passed to Docker as `--user :`.

To see that the isolation is real rather than assumed, the example config asks
the mock for `hostname; id -u; cat /etc/hostname`. The tool card then shows the
container's hostname, uid 0 and its own PID namespace — from a session whose hub
is an ordinary process on the host:

```
command   hostname; id -u; cat /etc/hostname
pid       10
stdout    66485abf0d8d          ← the container
          0
          66485abf0d8d
```

`--rm` means stopping the session removes the container, which is why the
`docker ps` output is empty the moment the run finishes. For Docker Desktop,
use `host.docker.internal` as `connectHost` and add
`--add-host=host.docker.internal:host-gateway` to the command template.

### With a real model instead of the mock

The same configuration, without `--mock`, talks to `api.deepseek.com`:

```sh
export DEEPSEEK_API_KEY=sk-...        # the key stays in your shell
node bin/simplex-hub.ts -c hub.config.docker-worker.jsonc --no-mock \
    --listen 0.0.0.0:8800 --panel-token dev --data-dir /tmp/docker-hub
```

then create the session with the `deepseek` profile — from the panel's *new*
form it would default to that profile anyway, since `deepseek` is the first
entry in `providerProfiles`:

```sh
curl -s -X POST http://127.0.0.1:8800/api/sessions \
  -H 'Authorization: Bearer dev' -H 'content-type: application/json' \
  -d '{"session":"real","spec":{"provider":"deepseek","model":"deepseek-flash"}}'
```

The key is forwarded with `-e DEEPSEEK_API_KEY` (no `=value`, so Docker reads it
from the environment of the process that runs `docker`), and the worker expands
`${DEEPSEEK_API_KEY}` from *its own* environment — which is the container's.
When it is missing, that is what you get, and it is worth recognising:

```
Worker: required credential variable is unset or empty
```

To check the forwarding on its own, before blaming the API:

```sh
docker run --rm -e DEEPSEEK_API_KEY --entrypoint sh simplex-hub-test:latest \
    -c 'test -n "$DEEPSEEK_API_KEY" && echo "the container can see the key"'
```

Two things to know before reading the result:

- **The model name is this project's alias, not the public one.** The plugin
  advertises `deepseek-flash` and `deepseek-v4-pro` and enables thinking mode
  (`llm/README.md`), which is not the `deepseek-chat` the public API reference
  lists. It is rendered straight into the worker's configuration, so if the
  endpoint rejects it, change it in the session's spec rather than in code.
- **A real model is slower in a way the panel is honest about.** There is no
  token streaming anywhere in this path — the worker carries whole messages — so
  the reply appears when it is finished. The activity cue shows that the run is
  waiting and explains that the reply appears when complete.

## Configuration

Copy [`hub.config.example.jsonc`](hub.config.example.jsonc) to
`hub.config.jsonc`, or pass any file with `--config`. Comments are allowed:
the reader strips `//` and `/* */` before parsing, and the file stays valid JSON
without them. Relative paths resolve against the configuration file's
directory; command-line paths resolve against the working directory.

| Key | Default | Meaning |
| --- | --- | --- |
| `listen.host`, `listen.port` | `127.0.0.1`, `8800` | panel and API listener |
| `dataDir` | `./data` | hub state, generated worker configs, logs, JSONL event logs, worker snapshots |
| `panel.token` | `""` | shared panel token; required for a non-loopback listener |
| `worker.bin` | `../build/bin/simplex_worker` | worker executable |
| `worker.promptsDir`, `worker.systemPromptFile` | `../build/bin/prompts`, `coding_agent.yaml` | default prompt for new sessions |
| `worker.threads`, `worker.maxExchanges`, `worker.eventCapacity` | `1`, `512`, `1024` | defaults copied into generated worker configurations |
| `worker.confirmationTimeoutMs` | `120000` | confirmation deadline written into the worker configuration |
| `worker.stopTimeoutMs`, `worker.sigtermGraceMs`, `worker.sigkillGraceMs` | `15000`, `5000`, `2000` | the stop escalation ladder |
| `worker.persistence` | `{enabled: true, readable: false}` | worker snapshot policy |
| `providerProfiles` | `deepseek`, `mock` | copied into the generated worker configuration; a session picks one by name |
| `launcher.kind` | `simplex-worker` | `simplex-worker` or `command` |
| `launcher.command`, `launcher.args` | `[]` | template and extra arguments for the `command` launcher |
| `launcher.config` | `hub` | who renders the worker configuration: `hub` or `launcher` |
| `launcher.cwd`, `launcher.pidFile` | `""` | working directory, and the pid file for a launcher that daemonizes |
| `mock.enabled`, `mock.listen`, `mock.profile`, `mock.scenario` | `false`, `127.0.0.1:0`, `mock`, `auto` | offline provider |
| `limits.transcriptEvents` | `5000` | envelopes retained per session for panel replay |
| `limits.logLines`, `limits.logBytes`, `limits.logFiles` | `500`, `8 MiB`, `2` | captured worker output |
| `limits.maxMessageBytes` | `32 MiB` | largest accepted WebSocket message |
| `limits.pingIntervalMs` | `30000` | worker connection ping interval; `0` disables |
| `limits.confirmIdentityHoldMs` | `15000` | how long a confirmation with an unverified worker identity is held before denial |
| `forceKillProcessGroup` | `false` | whether `SIGKILL` targets the worker's process group |

`npm start -- --help` lists the command-line overrides (`--listen`,
`--data-dir`, `--worker-bin`, `--prompts-dir`, `--panel-token`, `--mock`,
`--force-kill-process-group`, `--log-level`).

## Managing workers

The hub renders a complete worker configuration per session into
`<dataDir>/workers/<session>/config.yaml`, and then runs the configured
launcher. Two kinds ship:

- **`simplex-worker`** — `simplex_worker --config <generated> --session <id>
  --threads N`, which is exactly the command line `core/README.md` documents.
- **`command`** — a template for a wrapper script or a different front end:

  ```jsonc
  "launcher": {
    "kind": "command",
    "command": ["bash", "scripts/simplex-run.sh", "{session}", "--config", "{config}"],
    "args": ["--endpoint", "{endpoint}", "--token", "{token}"],
    "config": "hub"
  }
  ```

  Placeholders are `{session}`, `{config}`, `{data_dir}`, `{session_dir}`,
  `{endpoint}`, `{confirm_endpoint}`, `{token}`, `{threads}`, `{worker_bin}`,
  and `{prompts_dir}`. An unknown placeholder fails the spawn by name instead of
  being passed through. A launcher that daemonizes must set `launcher.pidFile`,
  because the pid the hub spawned would then name a short-lived wrapper.

Stopping a worker is **protocol first**: the hub sends the worker's own
`shutdown` signal and waits `worker.stopTimeoutMs`, then `SIGTERM` for
`worker.sigtermGraceMs`, then `SIGKILL` for `worker.sigkillGraceMs`. That order
is what makes an orphan controllable — a hub that restarted without its process
table can still stop a worker it can reach over the socket.

*Force kill* in the panel skips straight to the signal and, by default, targets
the whole process group, which also reaches descendants the worker itself does
not promise to terminate. `forceKillProcessGroup` applies the same behaviour to
the automatic escalation; it is off by default.

If the hub is killed, its workers keep running and reconnect when a hub comes
back on the same address. The restarted hub restores sessions, tokens, and
process records from `hub.json`, adopts a worker whose pid *and* `/proc` start
time still match, and marks anything else as unattached. Nothing is signalled on
a pid alone.

## Files on disk

```
<dataDir>/
  hub.json                        sessions, tokens, process records
  workers/<session>/config.yaml    generated worker configuration
  workers/<session>/worker.log     captured worker output (rotated)
  events/<session>.jsonl           worker events the hub received
  sessions/<session>/state.json    the worker's own snapshot (authoritative)
  sessions/<session>/readable.md   optional human-readable copy
```

Conversation state lives in the worker's snapshot, not in the hub. The panel can
read it; nothing can replace, edit, or reset it.
When a worker is connected, the panel also requests a bounded, display-only
history projection of its turns. This restores the conversation after a panel
reload or hub restart without copying the worker's full state into hub storage.
Long turns are paged, and the hub logs only small cursor markers for the replies.

## Tests

```sh
npm run typecheck   # tsc over the server, the shared protocol, the panel, the browser tests
npm test            # unit and integration tests, no build required
npm run build       # bundle the panel into web/dist
npx playwright test # the panel in a browser Playwright brings with it
npm run test:e2e    # end-to-end against build/bin/simplex_worker (skipped if absent)
```

The end-to-end tests drive the real binary through the hub and the offline mock:
a full loop with a real tool call and confirmation, and a crashed hub whose
worker is adopted by the next hub. Set `SIMPLEX_WORKER_BIN` to test a different
build.

Overlays — menus, dialogs, popovers, tooltips, tabs — are Radix primitives, and
the reason is specific rather than fashionable: the old panel hand-wrote a focus
trap and a document-level key handler, and four of its interaction defects came
out of that one piece of code (a dialog whose initial focus landed on "hide", a
minimised dialog that reopened itself, buttons disabled forever after a failed
decision, and a trap spanning a stack of coexisting modal dialogs). Radix
supplies that machinery correctly, and the wrappers in `web/src/ui` exist so the
panel has one place where a menu looks like a menu.

The panel renders model output as markdown through `react-markdown`, which
builds elements rather than markup — so "show the model's markdown" and "never
write panel markup as HTML" are the same code path, not a trade. Inline HTML is
deliberately not parsed. Syntax highlighting is `rehype-highlight` over
highlight.js's *common* language set, which is a fixed ~50 kB gzipped of the
bundle and cannot be trimmed by configuration, because the plugin imports that
set statically. Radix and lucide together add about 42 kB gzipped on top of
that. The whole panel is ~230 kB gzipped, which for a tool that runs on loopback
is not a constraint anyone is paying for.

The panel is one page, built from `web/src` into `web/dist`, and the hub serves
that directory as its front door. There is no separate panel deployment and no
second copy of it: `npm run build` is the whole step, and a hub started without
it answers `503` with the command rather than a bare `404`.

`npm run dev:panel` runs Vite against a hub on `127.0.0.1:8800` when you want to
iterate with hot reload; the same proxy is configured for `vite preview`, which
is what the browser tests load. Both forward `/api` and the `/panel/ws` upgrade
and deliberately do **not** rewrite `Host` — see `vite.config.ts` for why
`changeOrigin` is the one setting that breaks the hub's cross-site check.

The panel's browser tests run against `test/browser/stub-hub.mjs`, a server that
speaks the panel protocol and can be told to emit an event, raise a confirmation
or restart with a new transcript epoch — none of which a real hub can be asked
to do on cue. It is deliberately not a mock of the hub's logic: ordering,
replay cursors and epochs are real, because those are what the tests are about.
It does repeat one piece of the hub on purpose: the `Origin`/`Host` check on the
panel upgrade, so that a proxy misconfiguration fails here rather than only
against the real thing.

To check the panel against a real hub and a real worker:

```sh
npm run build
node bin/simplex-hub.ts --mock --listen 127.0.0.1:8899 --data-dir /tmp/hub-check &
npm run check:panel
```

That creates a session, starts its worker, sends a message, answers the tool
approval the mock provider asks for, and prints the transcript the panel
rendered next to the hub's own counters. The browser loads the hub's own front
page over the hub's own socket — no build server in between — so it is the
deployment path being checked, not a preview of it.

### Continuous integration

Three jobs in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) cover this
package. None uses the C++ build images: those pin a compiler and Boost, carry
no Node, and are digest-pinned, so a Node install there would enter the plugin
ABI fingerprint the C++ jobs depend on.

| Job | Subject | Notes |
| --- | --- | --- |
| `hub-test` | the suite and the type checker on the declared Node floor (22.18) and the current release (24) | no C++ tree needed; drives stand-in workers over real WebSockets |
| `hub-panel` | the panel build and its browser tests | Node 24 only: Vite and Vitest both require more than the hub's floor, and installing a browser needs root |
| `hub-e2e` | the hub against the *staged release* worker from `portable-release` | the first workload that runs a release binary rather than a ctest executable |

Between them they also assert things a reader might otherwise assume:

- **Protocol drift.** `test/protocol-drift.test.js` parses
  `core/docs/worker-protocol.md` and fails when core adds an event, a signal, an
  input operation, or an option category the hub does not know about. A hub that
  silently rendered a new event as "unknown" would otherwise stay green.
- **Protocol constants.** `test/protocol-constants.test.js` fails if the hub's
  announced version, the version stamped on every panel message, and
  `shared/protocol.ts` ever disagree — and fails if a panel module writes a
  version down instead of importing it, which is how the old panel's copy
  drifted.
- **Panel integrity.** `test/panel-assets.test.js` checks that the entry
  document references only files that exist, that every relative import in the
  panel resolves, that both theme token sets declare the same names, and that
  nothing writes markup as HTML.
- **Panel accessibility.** `test/browser/panel-contrast.spec.ts` computes
  contrast from the rendered page in both themes, using the browser's own colour
  parser and the WCAG formula, and audits the structure a screen reader needs:
  a name on every control, one `h1`, no skipped heading level, landmarks, and no
  icon that says nothing.
- **The panel in a browser.** `npx playwright test` builds the panel, serves it
  through `vite preview` against a scripted hub, loads it in the browser
  Playwright installs, and fails on a console error. A separate CI step starts
  the real hub and asks *it* for `/`, because a preview server cannot notice a
  hub whose static root and build output disagree.
  The interaction suite is the one that matters most for review: each of its
  tests closes a numbered defect from the plan, and most are written so that
  they fail against the behaviour the old panel had.
- **The panel's own store.** `test/panel-store.test.js` covers the behaviours
  the rewrite deliberately changed — a replayed transcript is merged rather than
  substituted, a session list never deletes what it omits, a new transcript
  epoch resets the cursor, a log tail replaces rather than appends, and a
  refused input goes back to the composer. It needs no browser because the
  store is the vanilla Zustand store: no React, no DOM.
- **The transcript's derivations.** `test/panel-transcript.test.js` covers the
  two pure modules that read what the worker sent: `rounds.ts`, which decides
  what belongs to which turn, and `toolOutput.ts`, which reads the structure out
  of a tool result's prose. Its fixtures use the payload shape a real session
  produced, not the shape the protocol document describes — the two differ for
  tool results, and reading only the documented one shows no output at all.
- **What the command palette offers.** `test/panel-palette.test.js` checks the
  entries as a function of the store's shape — no "cancel the run" when no run
  is active, no per-session entries when no session is selected — because the
  offer is the interesting part, and a command that exists and then refuses is
  worse than one that is not there.
- **No markup from untrusted text.** `test/panel-assets.test.js` scans the panel
  for `innerHTML` and its relatives, `dangerouslySetInnerHTML` included,
  and fails if a raw-HTML plugin is added to the markdown renderer. The
  behavioural half of the same claim is the browser test that feeds a model
  response a `<script>` tag and a `javascript:` link and asserts neither becomes
  an element.

Not covered by CI: a real provider (the offline mock stands in), `wss` and
reverse-proxy behaviour, long-running sessions, and operating systems other than
the runner's Ubuntu.

## Documentation

- [hub/docs/hub-protocol.md](docs/hub-protocol.md) — the panel/API protocol.
- [hub/docs/worker-adapter.md](docs/worker-adapter.md) — how the hub implements
  the worker protocol, requirement by requirement, including the parts it
  deliberately leaves to the deployment.
- [core/docs/worker-protocol.md](../core/docs/worker-protocol.md) — the worker
  contract itself, which remains authoritative.

## Security notes

- A payload channel is an approval authority: anyone who can send a payload can
  select `confirmation.mode: approve` and authorize every tool call that needs
  confirmation. The hub defaults to a loopback listener, refuses a non-loopback
  listener without `panel.token`, and refuses cross-origin WebSocket upgrades.
- The hub has no user accounts or roles. One shared token guards the browser
  surface. For anything beyond a trusted machine, put an authenticating reverse
  proxy in front and keep the hub on a private interface.
- `wss://` is not implemented; terminate TLS at the proxy.
- Worker session tokens are bearer credentials stored in `hub.json` and in the
  generated worker configuration. Treat `<dataDir>` as sensitive.
- The hub cannot prove that a payload was admitted, executed, or persisted. It
  shows what it observed and marks the rest unknown, and it never resends
  automatically.
- The strongest boundary here is the container, not the token: run sessions in
  `docker/Dockerfile.hub-test` (above) and the worst case is a container you
  throw away. `run_command` is `require_confirm`, so every command is shown in
  the panel before it runs — but a confirmation is a decision, not a sandbox.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| A session shows *worker not connected* forever | wrong `worker.bin`, missing `plugins/` next to the binary, or a startup failure visible in the Process → Logs tab |
| `cannot spawn ... ENOENT` | `worker.bin` does not exist; check the path after path resolution |
| The worker starts and exits immediately | the provider profile is missing a credential (`DEEPSEEK_API_KEY`), or another worker already owns the session lock |
| A confirmation is denied with "identity mismatch" | a worker the hub does not know opened a confirmation for that session; check the event connection log |
| A confirmation is denied after ~15 s | the event connection never identified the worker; check that the event socket reconnected |
| Log lines are missing in the panel | the in-memory ring is bounded (`limits.logLines`); the full file is `workers/<session>/worker.log` |
| The panel returns 401 | `panel.token` is set; supply it with `?token=...` in the URL once |
