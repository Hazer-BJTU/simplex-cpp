# Deploy the Hub and configure a worker

The Hub runs independently of workers. It owns the configuration library,
session routing tokens, launch supervision, event transcript, and plan storage.
Worker conversation state remains worker-owned.

## Install a local worker

After installing the Hub npm package, run the standalone installer on the Hub
host before starting the server:

```sh
simplex-hub install-worker --platform linux-amd64-standard --directory "$HOME/.simplex/worker" --update-path
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
or set `management.executable` to the absolute installed `bin/simplex` path
(`launcher.command[0]` for a legacy command profile).
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

New `local` launch files bind `simplex run` and `simplex manage` to the same
local installation. Set `management.executable` to an absolute `bin/simplex`
path if it is not on the Hub's PATH, and `management.cwd` to the working directory.
The default target root is `<dataDir>-workers`, separate from Hub records;
manager-owned allocations provide SQLite Inspector reads. Set the worker's
workspace hint separately if the model should know that path. Existing saved
launch files retain their previous command and storage policy.

Each session retains copies of the selected files. The
[configuration library reference](../hub/configurations.md) describes managed
endpoint fields, launcher options, snapshot replacement, and the configuration API.

## Panel controls and diagnostics

The conversation and input share a centered reading area. Tools show compact
summaries; open a tool to inspect its arguments and results. Reasoning and
**Error details** expand independently. Raw protocol events and per-turn token
counters are not rendered in the conversation; **Inspector** retains connection,
worker, process, replay and snapshot diagnostics. The small input-area usage
indicator always describes the last response that reported token usage.

The input always sends a message; there is no command mode or Alt+Enter toggle.
**Skills** inserts reusable instructions. **Confirm** and **Model** configure
only the next request, including compaction, without changing an active run.
To continue a task, send a message such as “Continue”.

The inward-arrow icon beside the token meter has the tooltip **Compact**. It
archives the conversation and saves a summary as the worker's new context,
without submitting or clearing your draft or skill references. It requires a
connected, idle worker and Hub with compaction support; the worker validates
that it has a settled conversation to compact. The icon spins while waiting
for admission or compacting, and repeated submission is disabled. The result
or error appears in the conversation; use **Cancel run** to cancel execution.
History synchronizes automatically. Failure notices offer recovery actions;
**Refresh conversation** also remains in the global Ctrl/Cmd+K palette for
manual recovery, separate from message input.

During a request, **Send** becomes **Cancel run** in the same position. One click
shows **Cancelling…** until the worker responds or its connection changes. Tools
may still finish before an interruptible boundary; this does not stop the worker
process. The session header's **Stop** controls that process separately. A lost
connection does not establish that execution stopped.

Approval dialogs identify their source session and, for subagents, their parent.
Long arguments scroll while decision buttons stay reachable. A locally expired
approval disables those buttons while awaiting the Hub's authoritative outcome.
Light/dark themes, keyboard focus and reduced-motion settings apply throughout.

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

## Predefined skills in messages

The panel's **Skills** button inserts a literal `@{skill:ID}` reference at the
message caret. You can also type the prefix for keyboard completion, move or
delete a reference as ordinary text, and insert several references. Enter or Tab
accepts a completion; Escape dismisses it.

Skills live on the **Hub host**, under `<dataDir>/skills/` (by default
`~/.simplex/hub/skills/`), independently of worker directories. Put a UTF-8 YAML
file there, using a filename such as `code-review.yaml`:

```yaml
name: code-review
title: Code review
description: Review changes for correctness and maintainability.
keywords: [review, correctness, testing]
text: |
  Review the changes against the user's requirements.
  Prioritize correctness, regressions, and missing validation.
  Explain actionable findings with their location and impact.
```

`name`, `title`, `description`, and `text` are required nonempty strings;
`keywords` is an optional list of search terms. Metadata fields are single-line;
only `text` is inserted into the message. IDs are filename stems containing
ASCII letters, digits, `_`, or `-`, at most 128 characters. `template` is reserved
for the template endpoint. Files must be regular files, at most 64 KiB. Aliases,
unknown fields, and symbolic links are rejected. The library accepts up to 256
files. Invalid files are reported individually, without preventing Hub startup.
The packaged `code-review` example is created on first access if absent; existing
files are never overwritten. Copy it to a new filename to create another skill.

For example, `Please follow @{skill:code-review} for this change.` expands the
reference **at that position**. Whitespace around it remains unchanged. Use
`\@{skill:code-review}` to send the marker literally. Skill bodies are never
expanded recursively. A message accepts at most 16 references, with a 256 KiB
UTF-8 limit on the expanded text; ordinary messages without references retain
the existing transport limits.

The panel loads a content snapshot when it resolves a reference. **Preview
message** shows the exact expanded text. Reopening or refreshing the library
checks revisions; **Refresh skill text** explicitly replaces the selected
snapshots. Editing a file never silently changes an already-loaded instruction.
Missing, invalid, unfinished, or still-loading references block message sending.

Send transmits ordinary user text using the existing worker protocol. Skills do
not change confirmation policy, gain system-prompt priority, or install plugins.
They become part of the conversation and are persisted and compacted normally.
The Compact action does not expand or send the message draft. Successful
message submission clears the draft and its references; an explicit rejection
restores the original markers and snapshots. A late rejection offers recovery
without replacing a newer draft. This recovery state is browser-local; reloading
the page shows the actual expanded conversation, not editable skill references.
