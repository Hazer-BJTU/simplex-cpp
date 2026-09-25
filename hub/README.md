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

- Node.js 20.11 or newer (developed on 24).
- A built worker binary — `build/bin/simplex_worker` plus its `plugins/` and
  `prompts/` directories. Build it with the repository's normal CMake flow.
- One runtime dependency: [`ws`](https://github.com/websockets/ws).

## Quick start

```sh
cd hub
npm install
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

## Tests

```sh
npm test          # 150 unit and integration tests, no build required
npm run test:e2e  # end-to-end against build/bin/simplex_worker (skipped if absent)
```

The end-to-end tests drive the real binary through the hub and the offline mock:
a full loop with a real tool call and confirmation, and a crashed hub whose
worker is adopted by the next hub. Set `SIMPLEX_WORKER_BIN` to test a different
build.

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
