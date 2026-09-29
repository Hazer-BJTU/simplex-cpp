# simplex hub

A Node.js server that runs `simplex_worker` sessions and gives them a browser
panel.

The hub launches workers, collects their events, displays tool confirmations,
and retains process output. The old one-to-one terminal server `simplex_shell`
is deprecated source only and is no longer built or installed.

The hub implements the server side of
[`core/docs/worker-protocol.md`](../core/docs/worker-protocol.md). The worker and
hub share the direct session-root persistence layout described below.

## Requirements

- Node.js 22.18 or newer (developed on 24). The floor is a functional
  requirement rather than a conservative one: the hub loads
  [`shared/protocol.ts`](shared/protocol.ts) directly through Node's TypeScript
  type stripping, which became the default in 22.18. Nothing is compiled on the
  server side.
- A built worker binary — `build/bin/simplex_worker` plus its `plugins/` and
  `prompts/` directories. Build it with the repository's normal CMake flow.
- Runtime dependencies: `ws` for WebSockets and `yaml` for preserving operator
  configuration and comments. Everything else in `package.json` is a development dependency.

## Quick start

```sh
cd hub
npm install
npm run build        # bundles the panel into web/dist
npm start
```

Then open <http://127.0.0.1:8800>. Open **Configurations**, edit the default
worker YAML and inspect the local launch template. Create a session, select
both saved configurations, and press *Start worker*. The local template uses
`simplex run`; install `simplex` on PATH or edit its executable path.

Configuration files and session data default to `~/.simplex/hub`; use
`--data-dir` to choose another root. See [Configuration UI and lifecycle](docs/configurations.md)
for editing, endpoint discovery, Docker launch profiles and snapshot reuse.

The default worker template uses `YOUR_*` placeholders for the provider, plugin,
model and API endpoint. Fill them before starting a worker. Export the key the
template refers to; the worker expands `${MODEL_API_KEY}` at startup:

```sh
export MODEL_API_KEY=your-key
```

### In a disposable container

This is the way to drive the hub by hand. A session can propose arbitrary
commands through the process tools and, once confirmed, they run wherever the
worker runs. The container limits access to host files unless volumes are
mounted:

```sh
docker build -f docker/Dockerfile.hub-test -t simplex-hub-test .
docker run --rm --init -p 127.0.0.1:8800:8800 simplex-hub-test
# → http://127.0.0.1:8800/?token=simplex-hub-dev
```

It carries the worker (Debug, built in the image), the hub, its dependencies,
and the offline mock provider. In **Configurations**, create a launch file and
a worker file using **Current Hub deployment** as the template source, save both,
and select them for your session. This uses the image's binary and mock endpoint
without a model key. Add
`-e DEEPSEEK_API_KEY=sk-...` to use a real provider as well — a session chooses
its profile either way — and `-v simplex-hub-data:/data` to keep sessions after
the container is gone. `docker run -it --rm simplex-hub-test bash` gives a shell
in the same tree. Volumes and bind mounts retain worker-written state on the
host after the container exits. Treat mounted workspaces and session data as
persistent, writable data; keep unrelated host files outside those mounts.

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

Create both configuration files from the **Current Hub deployment** template.
With `--mock`, its worker file selects the `mock` provider; choose `mock-auto`
as the model in that YAML if desired. Select those files for a session, start it,
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
# From the repository root:
docker build -f docker/Dockerfile.hub-test -t simplex-hub-test .
cd hub
npm run build
node bin/simplex-hub.ts -c hub.config.docker-worker.jsonc --mock \
    --listen 0.0.0.0:8800 --panel-token dev --data-dir /tmp/docker-hub
# then: http://127.0.0.1:8800/?token=dev
```

In the configuration editor, select **Current Hub deployment** and save one
launch configuration and one worker configuration. Select both for your session.
The legacy startup file below remains the source of these deployment templates;
subsequent edits take place in the saved library files.

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
   captured log are all named by *absolute host paths*. Only the current
   session directory is mounted read-write at the same path, and `config.yaml`
   is mounted again as a read-only file. This keeps tools from rewriting durable
   launch settings or another session's data. The prompt is not mounted at all:
   the worker reads it from its own installation directory inside the image, and
   the generated `worker.system_prompt_file` is a relative path that names the
   same file on both sides.

The test launcher uses `--user 0:{gid}`: workers and their tools run as root inside
the container. Each container has its own `/root/workspace`, created by the
image and selected as both the working directory and the model's workspace
hint. No host workspace is mounted. Stopping and removing the container discards
workspace files; session state remains in the mounted session directory.
The worker starts with umask `0002`, so directories it creates under the
mounted session path are group-writable by the hub process. Worker files have
root ownership and the hub's primary GID; the host hub can still remove the
session tree. A tool that deliberately changes permissions inside the mounted
session path can defeat this cooperative policy.
After building the image, run `SIMPLEX_DOCKER_WORKER_TEST=1 node --test
test/e2e/docker-worker.test.js` from `hub/` to verify a real compact, stop,
and session deletion as the current non-root host user.

To see that the isolation is real rather than assumed, the example config asks
the mock for `hostname; id -u; cat /etc/hostname`. The tool card then shows the
container's hostname, UID 0 and its own PID namespace — from
a session whose hub is an ordinary process on the host:

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

then create both saved configurations from **Current Hub deployment** and
select them for the session. With `--no-mock`, the worker template selects
the first configured provider, normally `deepseek`:

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
`~/.simplex/hub/hub.config.jsonc` (or `<dataDir>/hub.config.jsonc`), or pass
any file with `--config`. Comments are allowed:
the reader strips `//` and `/* */` before parsing, and the file stays valid JSON
without them. Relative paths resolve against the configuration file's
directory; command-line paths resolve against the working directory.

| Key | Default | Meaning |
| --- | --- | --- |
| `toolRequests.host`, `toolRequests.port` | inherits `listen.host`, `8801` | independent worker tool-request listener |
| `toolRequests.timeoutMs`, `toolRequests.maxConnections` | `120000`, `128` | hard per-connection deadline and global upgraded-connection limit |
| `listen.host`, `listen.port` | `127.0.0.1`, `8800` | panel and API listener |
| `dataDir` | `~/.simplex/hub` | hub state, generated worker configs, logs, JSONL event logs, worker snapshots |
| `panel.token` | `""` | shared panel token; required for a non-loopback listener |
| `worker.bin` | `../build/bin/simplex_worker` | worker executable |
| `worker.systemPromptFile` | `prompts/coding_agent.yaml` | default prompt for new sessions, relative to the worker's installation directory |
| `worker.threads`, `worker.maxExchanges`, `worker.eventCapacity` | `1`, `512`, `1024` | defaults copied into generated worker configurations |
| `worker.hubRemoteCall` | `true` | include the optional remote-call toolset config for new sessions |
| `worker.confirmationTimeoutMs` | `120000` | confirmation deadline written into the worker configuration |
| `worker.stopTimeoutMs`, `worker.sigtermGraceMs`, `worker.sigkillGraceMs` | `15000`, `5000`, `2000` | the stop escalation ladder |
| `worker.persistence` | `{enabled: true, readable: false}` | worker snapshot policy |
| `worker.memoryRetention` | `{maxArchives: 5}` | compact archive cleanup defaults for new worker configs; zero disables cleanup |
| `providerProfiles` | `deepseek`, `mock` | copied into the generated worker configuration; a session picks one by name |
| `launcher.kind` | `simplex-worker` | `simplex-worker` or `command` |
| `launcher.command`, `launcher.args` | `[]` | template and extra arguments for the `command` launcher |
| `launcher.cwd`, `launcher.pidFile` | `""` | working directory, and the pid file for a launcher that daemonizes |
| `mock.enabled`, `mock.listen`, `mock.profile`, `mock.scenario` | `false`, `127.0.0.1:0`, `mock`, `auto` | offline provider |
| `limits.transcriptEvents` | `5000` | envelopes retained per session for panel replay |
| `limits.logLines`, `limits.logBytes`, `limits.logFiles` | `500`, `8 MiB`, `2` | captured worker output |
| `limits.maxMessageBytes` | `32 MiB` | largest accepted WebSocket message |
| `limits.pingIntervalMs` | `30000` | worker connection ping interval; `0` disables |
| `limits.confirmIdentityHoldMs` | `15000` | how long a confirmation with an unverified worker identity is held before denial |
| `forceKillProcessGroup` | `false` | whether `SIGKILL` targets the worker's process group |

`npm start -- --help` lists the command-line overrides (`--listen`,
`--data-dir`, `--worker-bin`, `--panel-token`, `--mock`,
`--force-kill-process-group`, `--log-level`).

## Managing workers

On the first start, the hub writes the session configuration to
`<dataDir>/sessions/<session>/config/config.yaml`. Later starts and restarts reuse
this file, including hand-written YAML and comments. Hub default changes and
configuration fields in a later launch spec do not overwrite it. Edit the saved
file while the worker is stopped to change model, prompt, persistence or other
worker settings. Launch-only `threads`, `env` and `extraArgs` still come from the
session spec.

New DeepSeek sessions default to a separate `providers.modality_assist` entry,
copied from `providerProfiles.deepseek`, and set
`modality_assist_model: modality_assist`. A session-level `model` override
changes only the driver; the assistant keeps the model in the original profile.
Other provider sessions and mock sessions do not acquire a DeepSeek dependency.
For a new session using another provider, set `modalityAssistProvider` in the
session spec to a configured, non-mock profile name to opt in. Set it to `null`
to disable the DeepSeek default for a new DeepSeek session. The assistant always
gets a separate provider entry, and its credentials must be available to the
worker at startup. Existing session files are not backfilled; edit their
top-level `providers` and `modality_assist_model` fields while the worker is
stopped to enable, change or remove the assistant.
After a restart, the session spec reports the saved worker configuration's
`modality_assist_model` provider key (or `null` when absent), even if Hub defaults
or the original session spec have since changed.

Before each launch the hub refreshes only `persistence.directory` (the direct
session root), `client.endpoint`, `security.confirmation.endpoint`, and
`hub_remote_call.endpoint` when configured (including session authentication
tokens), and the active mock provider's dynamic
`endpoint.base_url`. Provider credentials and other operator fields remain intact.
Malformed saved YAML or invalid persistence child paths fail startup without
replacing the file. Updates are published with an atomic rename.
The hub owns this configuration for every launcher. A custom launcher must use
the supplied `{config}` as its authoritative worker configuration. Refreshes
preserve an existing file's owner, group and permission bits; if the worker runs
under another UID, the operator must grant it read access. Native workers and
their tools run with their process's filesystem authority, so a same-UID worker
can edit this saved configuration. Use a separate UID or a read-only mount when
worker-side changes must not become durable operator settings.

The hub then runs the configured launcher. Two kinds ship:

- **`simplex-worker`** — `simplex_worker --config <generated> --session <id>
  --threads N`, which is exactly the command line `core/README.md` documents.
- **`command`** — a template for a wrapper script or a different front end:

  ```jsonc
  "launcher": {
    "kind": "command",
    "command": ["bash", "scripts/simplex-run.sh", "{session}", "--config", "{config}"],
    "args": ["--endpoint", "{endpoint}", "--token", "{token}"]
  }
  ```

  Placeholders are `{session}`, `{config}`, `{data_dir}`, `{session_dir}`,
  `{endpoint}`, `{confirm_endpoint}`, `{tools_endpoint}`, `{token}`, `{threads}`, and
  `{worker_bin}`. An unknown placeholder fails the spawn by name instead of
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

The panel composer has **Message** and **Command** modes. Use `Alt + Enter` to
switch modes. Message mode sends text and attached
references only while a worker is connected. During an active run, its Send
button becomes **Cancel run**; an unsent draft stays in the composer for later.
Command mode accepts command names without a `/` prefix: type a name prefix,
use `Tab` to complete it, and press `Enter` to run it. Executing a command
leaves the composer in Command mode. Switching modes keeps an
unsent message draft. **Refresh conversation** recovers the hub's retained event
transcript
and asks a connected worker for its simplified conversation history. If the
worker is offline, the hub transcript can still refresh; worker history will
be requested after reconnection. The command does not send a user message.
**Continue run** is always listed in Command mode. It is available when a worker
is connected and no run is active; it asks the worker to continue from its
current internal state without sending a new message, and keeps the draft.
The worker requires an existing conversation turn and reports an error if there
is none. The panel shows the continued run without a user-message bubble.
**Compact context** archives the conversation and replaces the worker's context
with a durably saved summary. It is enabled only when the connected worker and
hub advertise support and no run is active. The worker requires persistence and
a settled conversation. The panel displays the summary separately, refreshes
worker history, and preserves any unsent draft. Send a new message after success;
there is no turn to continue. Cancellation and failure preserve the old history.
The command shows the worker's actual archive-retention policy when reported.
When a run fails, its transcript shows a visible failure notice with technical
details available on demand. A model-request failure suggests **Continue run**
only while it remains the latest run of the same connected worker and the
worker reported that its settled state permits continuation. Older failures
keep their historical outcome without a stale retry instruction;
older workers without that classification receive general failure wording.

## Files on disk

```
<dataDir>/
  hub.json                        sessions, tokens, process records
  sessions/<session>/
    config/config.yaml            persistent worker configuration
    state/state.json              authoritative worker snapshot
    state/readable.md             optional human-readable copy
    memory/<ordinal>-<time>-<run>/state.md   compact archives
    logs/worker.log                captured worker output (rotated)
    events.jsonl                  worker events the hub received
    session.lock                  exclusive worker ownership
```

Conversation state lives in the worker's snapshot, not in the hub. The panel can
read it; nothing can replace, edit, or reset it.
Deleting an inactive session removes its directory entries, including configuration,
archives, logs and tool-created files inside it. Recreating the same ID starts
fresh. State and memory
subdirectory names may be configured with `persistence.state` and
`persistence.memory`; both are relative to `persistence.directory`, without any
additional session-ID suffix. Snapshot inspection follows the saved `state` path.
Child-path validation rejects absolute paths and lexical `..` traversal, but
does not resolve symlinks. A symlink within the session can point state or
memory outside it; these paths are not a filesystem sandbox. The operator must
trust or constrain workers with filesystem access.
After a successful compact, worker-owned cleanup applies the configured count
limit (5 by default) to recognized archives, preserving the current archive.
Cleanup failures are visible beside the saved summary. Failed/cancelled attempts,
unexpected files and empty directories can remain, so limits are cleanup targets
rather than disk quotas. The hub never deletes a remote path from an event.
The old `workers/`, `events/`,
and session-ID-appending worker layouts are not migrated or read automatically.
This is an incompatible layout change: migrate standalone worker configurations
that previously set a root directory and relied on the worker appending the
session ID. Set `persistence.directory` to the session's direct root and choose
relative `persistence.state` and `persistence.memory` directories.

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
a full loop with a real tool call and confirmation, cancellation while a tool
confirmation is pending, and a crashed hub whose
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
- The strongest boundary here is the container, not the token. An unmounted
  disposable container can be discarded, but `/data` and workspace mounts
  survive it and remain writable according to their mount permissions.
  `run_command` is `require_confirm`, so every command is shown in
  the panel before it runs — but a confirmation is a decision, not a sandbox.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| A session shows *worker not connected* forever | wrong `worker.bin`, missing `plugins/` next to the binary, or a startup failure visible in the Process → Logs tab |
| `cannot spawn ... ENOENT` | `worker.bin` does not exist; check the path after path resolution |
| The worker starts and exits immediately | the provider profile is missing a credential (`DEEPSEEK_API_KEY`), or another worker already owns the session lock |
| A confirmation is denied with "identity mismatch" | a worker the hub does not know opened a confirmation for that session; check the event connection log |
| A confirmation is denied after ~15 s | the event connection never identified the worker; check that the event socket reconnected |
| Log lines are missing in the panel | the in-memory ring is bounded (`limits.logLines`); the full file is `sessions/<session>/logs/worker.log` |
| The panel returns 401 | `panel.token` is set; supply it with `?token=...` in the URL once |


The message composer places **Model** beside **Confirm**. It fetches the connected
worker's model options once and caches the advertised choices for that worker
ID. Selections travel in the next message, continuation, or compact payload;
they never mutate an active run through a signal. A new worker gets a new cache.
An explicit `input_rejected` with `code: "invalid_options"` refreshes the options
and restores the provider's effective selections; ordinary input failures do
not refresh them. The header no longer exposes Status or Options buttons.


Message and Command modes share the same input layout and Send button.
Command mode keeps Confirm and Model visible but disabled; existing selections
still accompany commands that start a run. Alt+Enter changes the mode badge,
placeholder, and command suggestions without moving the toolbar. The mode badge
uses distinct colors and a short animation, disabled by reduced-motion settings.

A small usage line above the composer shows only the latest model response's
prompt tokens, generated tokens, and cache-hit percentage (`cache_hit / prompt`,
or zero for an empty prompt). Counts use decimal K/M/B units with one fractional
digit. Responses without cost leave the previous usage visible; counts are not
accumulated. Per-response and per-turn token details appear only with Technical
details enabled.


The confirmation trigger has a fixed width. Its label changes from Confirm to
Approve (red) or Deny (amber), so the active policy stays visible without adding
a warning row below the composer or changing the input area's height.

The composer header places an eight-segment request-size meter at a fixed
position immediately to the right of the Message/Command badge. It is based on the
last response's prompt plus generated tokens (cache hits are already included
in prompt). Each segment spans 128K = 131,072 tokens; the scale ends at 1M =
1,048,576 tokens. Completed bands are filled, the current band is proportional,
and the display remains full above 1M. The adjacent fraction identifies the
current band; zero belongs to the first band with an empty meter. This fixed
visual scale is not the provider's context-window limit. Numeric token counts
continue to use decimal K/M/B units.

## Worker remote tool requests

The hub binds a second WebSocket listener at `toolRequests.host:toolRequests.port`
(default port `8801`; empty host inherits `listen.host`). Port `0` selects an
ephemeral port, useful for embedded instances and tests. A nonzero tool port must
differ from the main port. This listener serves no panel, REST API, event stream,
or confirmation route. Deployments must make it reachable from workers; the
Docker worker example uses the same `worker.connectHost` for both ports.

New worker configurations include the optional `hub_remote_call` mapping by
default. Set `worker.hubRemoteCall: false` to disable it for new sessions. It contains the actual tool port,
`/agent/<session>/tools`, the session token, and `timeout_ms`. An enabled worker
constructs the intrinsic set and exposes the plan tool.
On restart only an existing mapping's URL/token is refreshed. Its timeout and
unknown fields are preserved; an absent mapping remains disabled even if hub
defaults change. A missing timeout in an enabled mapping is filled from hub
settings. A template launcher may also use `{tools_endpoint}`.

A worker appends a route such as `files/read` to the URL pathname, opens one
connection, sends one `tool_request`, and receives one `tool_response`. The `plan/read` and `plan/replace` routes operate on the current session plan
after verifying the live worker and active run. Unknown routes receive
`not_implemented`. Plans are saved atomically in the session root and pushed to
the panel, which offers a Plan tab beside Conversation only when the plan is nonempty. See the complete
[worker protocol](../core/docs/worker-protocol.md#remote-tool-requests).

`src/worker/tools.ts` owns authentication, framing, resource bounds, and shutdown.
`src/protocol/tool-requests.ts` owns envelope validation and the dispatch boundary.
Future operations must add explicit route registration, live-worker/operation
authorization, argument validation, response types, and tests at that boundary;
they must not derive executable commands or filesystem paths from route strings.
The session token alone is insufficient for plan operations: worker/run identity
is verified using the event connection. There is no implicit retry, replay cache,
confirmation bypass, or durable remote-call queue.
