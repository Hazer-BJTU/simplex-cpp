# Deploy the Hub and configure a worker

The Hub runs independently of workers. It owns the configuration library,
session routing tokens, launch supervision, event transcript, and plan storage.
Worker conversation state remains worker-owned.

## Install a local worker

After installing the Hub npm package, run the standalone installer on the Hub
host before starting the server:

```sh
simplex-hub install-worker --directory "$HOME/.simplex/worker" --update-path
# Open a new shell or source ~/.bashrc before starting/restarting the Hub.
```

This uses the latest stable official GitHub release by default; `--version`
selects a specific stable tag. The installer does not load startup configuration
or bind listeners. It installs locally, not into Docker containers or remote
hosts. See [installation](../getting-started/installation.md) for supported hosts,
replacement/version decisions, failure recovery, and optional PATH persistence.

Stop workers using the destination before replacing its installation. The Hub's
saved `local` launcher calls `simplex` through its inherited PATH; updating
`.bashrc` cannot change an already-running Hub. Restart from an updated shell,
or replace `launcher.command[0]` with the absolute installed `bin/simplex` path.
The installer does not rewrite saved launch configurations.

## Startup

```sh
export MODEL_API_KEY='your-api-key'
simplex-hub --data-dir "$HOME/.simplex/hub"
```

Useful startup options:

| Flag | Purpose |
| --- | --- |
| `--config FILE`, `-c` | Explicit Hub JSONC/JSON startup file |
| `--data-dir DIR` | Configuration and session root |
| `--listen HOST:PORT`, `-l` | Main listener; default `127.0.0.1:8800` |
| `--panel-token TOKEN` | Shared panel/API credential |
| `--worker-bin PATH` | Direct binary launcher path; saved command launchers use their own command |
| `--log-level LEVEL` | `trace`, `debug`, `info`, `warn`, `error`, or `silent` |
| `--version`, `-V` | Hub npm package version |
| `--help`, `-h` | Complete options |

The default startup file is `<dataDir>/hub.config.jsonc`, with a JSON fallback.
On first startup the Hub writes resolved defaults if no file exists. Later CLI
overrides do not rewrite an existing startup file. Listener changes require a
Hub restart.

## Listener configuration

A minimal custom startup file for a private-network deployment is:

```json
{
  "listen": {"host": "0.0.0.0", "port": 8800},
  "panel": {"token": "REPLACE_WITH_A_RANDOM_SECRET"},
  "toolRequests": {"host": "0.0.0.0", "port": 8801}
}
```

The main listener serves the panel, API, worker event sockets, and confirmations.
Port 8801 serves one-shot remote tool requests. The worker initiates outbound
connections to the Hub. Firewall both listeners appropriately. The Hub has no
native TLS listener; use a TLS reverse proxy for untrusted networks.

## Configure and start a session

Follow the [worker configuration guide](../getting-started/configuration.md)
to save a worker YAML with valid provider settings. Select it with a saved launch
file when creating a session, then start the worker and wait for connection.
The Hub validates configuration structure; the worker validates plugin availability
and uses the provider credentials at run time.

The default `local` launch file invokes
`simplex run --config {config} --session {session} --threads {threads}`.
Arguments are separate array entries, not implicitly shell-expanded. Use an
absolute executable path if `simplex` is not on the Hub's PATH. Set `launcher.cwd`
to the process working directory; an empty value uses the session directory.
Set the worker's workspace hint separately if the model should know that path.

Each session retains copies of the selected files. The
[configuration library reference](../hub/configurations.md) describes managed
endpoint fields, launcher options, snapshot replacement, and the configuration API.

## Advertised endpoints and reverse proxies

For explicit advertised addresses, set `worker.connectHost` or per-channel
`endpoints` in the launch file. A reverse-proxy example is:

```json
"endpoints": {
  "events": "wss://hub.example/worker",
  "confirm": "wss://hub.example/worker",
  "tools": "wss://hub.example/remote"
}
```

These are origin/prefix settings, not complete session URLs. The Hub appends its
session route and token. Route `/worker/agent/.../events` and `/confirm` to the
main listener, and `/remote/agent/.../tools/...` to the tool listener, stripping
the corresponding proxy prefix before forwarding.

## Stop, restart, and update configuration

A session gets `config/launch.jsonc`, `config/config.yaml`, and `config/source.json`
under `<dataDir>/sessions/<id>`. Starting it again reuses these snapshots. To use
new library settings, stop and disconnect the worker and explicitly apply both
saved files. Existing state and archives remain.

Stop through the Hub or send SIGINT/SIGTERM to a manually launched worker.
Shutdown cancels active work, waits for owned tasks, attempts persistence, and
cleans up process sessions. A running tool may delay it; force-killing bypasses
cleanup and cannot undo external effects.

On restart, `restore: if_present` restores saved state without starting inference.
Stored prompts remain; current skills and environment hints are refreshed.
Inspect [recovery phases](../architecture/agent-loop.md#recovery-phases) before
continuing interrupted work.

## Manually launched workers

For a session already registered with the Hub, prepare YAML with its actual
endpoints/token and run:

```sh
export MODEL_API_KEY='your-api-key'
simplex run --config /absolute/path/to/config.yaml --session demo --threads 2
```

`--session` must match the Hub route. Multiple executor threads do not permit
concurrent agent-loop invocations. See [the CLI reference](../reference/cli.md)
and [remote deployment](remote-worker.md) for flags and endpoint provisioning.

## Diagnostics

Check locally launched worker output at
`<dataDir>/sessions/<id>/logs/worker.log`. For a manual remote worker, inspect its
host's process logs. Startup failures commonly indicate an executable path,
shared-library, provider, credential, or session-lock problem. Repeated connection
attempts require checking listener reachability, routes, tokens, and TLS.

The [Hub panel protocol](../hub/hub-protocol.md) documents operator authentication
and session creation for non-browser clients.
