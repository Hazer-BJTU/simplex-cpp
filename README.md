# Simplex

[![CI](https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Hazer-BJTU/simplex-cpp)](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest)
[![npm](https://img.shields.io/npm/v/%40hazer-bjtu%2Fsimplex-hub)](https://www.npmjs.com/package/@hazer-bjtu/simplex-hub)
[![License](https://img.shields.io/github/license/Hazer-BJTU/simplex-cpp)](LICENSE)

**Understand every detail of an agent harness.**

Simplex is a lightweight, native C++ harness framework for LLM agents. It is
also a learning project: a working system you can explore to understand how to
build a minimal agent from scratch, from the first user request to model calls,
tool execution, cancellation, and persistent conversation state.

## Architecture

Each worker runs as its own process. The C++ core connects model providers,
tool and hook registries, the agent loop, and disk persistence. Tools and hooks
can be built in or loaded as dynamic plugins; model providers share a common
interface.

Workers connect over WebSocket to a separate Node.js Hub. The Hub manages
worker sessions and exposes a browser panel for messages, tool approvals,
configuration, and conversation history. The documented protocol lets other
servers interact with the same worker independently of the bundled Hub.

## Getting started

The simplest setup runs the worker and Hub on the same Linux host. You need:

- Linux x86_64 with glibc 2.34 or newer and OpenSSL 3 for the prebuilt worker.
- Node.js 22.18 or newer and npm for the Hub.
- A model endpoint and credentials supported by an installed provider plugin.

### 1. Install the worker

Download the worker `.tar.gz` archive and `SHA256SUMS` from the
[latest GitHub Release](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest).
In the download directory, verify and extract the archive:

```sh
sha256sum -c SHA256SUMS
tar -xzf simplex-worker-v*-linux-x86_64-glibc2.34.tar.gz
```

Keep the extracted directory intact: the worker locates its libraries, plugins,
schemas, and prompts relative to its executable. Add the extracted `bin`
directory to your shell's `PATH`, using its actual absolute path:

```sh
export PATH="/path/to/extracted-worker/bin:$PATH"
simplex run --help
```

Add the same export to your shell startup file if you want it to persist across
terminal sessions.

### 2. Install and start the Hub

The npm package includes the server and browser panel:

```sh
npm install -g @hazer-bjtu/simplex-hub
```

The default worker template reads its credential from `MODEL_API_KEY`. Set it
in the shell that starts the Hub; locally launched workers inherit that
process's environment:

```sh
export MODEL_API_KEY='your-api-key'
simplex-hub
```

Open <http://127.0.0.1:8800>. The default listener is local to your machine.
Configuration and session files are stored under `~/.simplex/hub`. To use a
different location, start the Hub with:

```sh
simplex-hub --data-dir /absolute/path/to/simplex-data
```

### 3. Configure the model and launcher

Open **Configurations → Worker configs → default**. This is an editable YAML
file. Replace its provider placeholders:

| Placeholder | What to enter |
| --- | --- |
| `YOUR_PROVIDER` | A configuration name of your choice; use the same name in `driver_model`. |
| `YOUR_PROVIDER_PLUGIN` | The name of an installed model-provider plugin. This selects the implementation, not the display name. |
| `YOUR_PROVIDER_BASE_URL` | The provider's HTTP(S) API base URL. |
| `YOUR_REQUEST_PATH` | The completion request path appended to the base URL's path prefix. |
| `YOUR_MODEL` | A model identifier accepted by that provider. |

Keep `api_key: ${MODEL_API_KEY}` to use the environment variable exported above.
If your endpoint uses different authentication, edit `endpoint.auth` to match.
Provider-specific generation settings belong in the provider's `config` mapping.
Save the worker configuration when ready.

Leave the `{{hub.*}}` connection markers in place for Hub-managed sessions.
The Hub fills in the event and confirmation endpoints and each session's
persistence directory at launch. The optional `modality_assist_model` and
`hub_remote_call` sections are commented out in the template; enable them only
when you want image-assistance or remote tools such as the shared plan.

Next, open **Launch configs → local**. Its command starts `simplex run` using
the installed worker. If `simplex` is not on the Hub's `PATH`, replace the first
entry of `launcher.command` with the absolute path to `bin/simplex`. Keep `run`
as the next argument. You can also set `worker.threads` here.

For a project-specific working directory, set `launcher.cwd` to that directory's
absolute path. Optionally set `worker.environment.workspace` in the worker YAML
to the same path so the model knows where it is working. The workspace field is
a prompt hint; it does not change the process directory or restrict file access.
Local tools execute with the worker process's permissions.

### 4. Create a session

Choose **Create session**, enter a session ID, and select the saved `local`
launch configuration and `default` worker configuration. Start the worker and
wait for its connection before sending a message.

Each session gets its own copies of the selected configuration files. You can
create additional named launch and worker configurations with **New from
template** and select them for other sessions.

## Using a session

Write your request in **Message** mode and select **Send**. The panel displays
model replies, tool activity, and approval requests. The **Model** control shows
options advertised by the worker's provider; selections take effect on the next
request, including a continuation or compaction.

The **Confirm** control selects how calls requiring confirmation are handled:

- **ask**: display an approval request and wait for your decision.
- **approve**: automatically approve calls that require confirmation.
- **deny**: automatically reject calls that require confirmation.

This choice applies to tools that require confirmation; it does not override
other tool trust policies. In **ask** mode, inspect the proposed call and approve
or reject it in the panel.

During an active run, **Send** becomes **Cancel run**. Click it once and wait for
the worker to reach a safe interruption point. A model request can be interrupted;
a tool already executing may finish before cancellation completes. Cancellation
does not undo completed tool effects.

### Commands

Press **Alt + Enter** in the composer to switch between Message and Command
modes. Commands have no `/` prefix. Type a prefix to see suggestions, use **Tab**
to complete, and **Enter** to execute. Switching modes preserves your message
draft.

| Command | Purpose |
| --- | --- |
| **Refresh conversation** | Reload the Hub's retained events and request conversation history from the worker. Use this to restore the display after reconnecting. |
| **Continue run** | Ask the worker to continue from its current internal state without adding a user message. Requires a connected, idle worker and an existing turn that can be continued. |
| **Compact context** | Archive the current conversation and replace its context with a saved summary. Requires an idle worker, persistence, and a settled conversation. |

A failed model request is shown as a failure notice. When the worker reports that
continuation is available, use **Continue run** to try again. After successful
compaction, send a new message to begin the next turn. The default compaction
retention policy keeps up to five recognized archives after successful cleanup.

### Stop, restart, and update configuration

Use the session's worker controls to stop or restart its process. A restarted
worker restores its saved conversation state; restoring state does not itself
start another agent run. Send a message or use **Continue run** when appropriate.

Editing a reusable configuration does not change an existing session's copy.
To apply changes, stop the worker, wait for disconnection, open
**Configurations**, select the saved launch and worker files, and choose
**Apply to …** for that session. Start the worker again afterward. Conversation
state is retained; prompt-file changes do not replace a system prompt already
restored from that state.

## Where data is stored

By default, the Hub uses the following layout:

```text
~/.simplex/hub/
├── hub.config.jsonc          # Hub startup settings
├── hub.json                  # Session metadata
├── configs/                  # Reusable launch and worker configurations
└── sessions/<session-id>/
    ├── config/               # This session's configuration copies
    ├── state/state.json      # Worker conversation state
    ├── memory/               # Compaction archives
    ├── logs/worker.log        # Worker process output
    └── events.jsonl           # Events received by the Hub
```

Deleting an inactive session removes its session data, including saved state
and archives. Reusing its ID afterward creates a fresh session.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Worker cannot start | Run `simplex run --help` in the Hub's environment, or set an absolute executable path in the launch configuration. |
| Worker exits immediately | Read the session's worker log. Check provider placeholders, plugin availability, model name, and credentials. |
| API key is missing | Export the variable before starting the Hub. If you changed its environment, restart the Hub and worker. |
| Worker cannot connect | Check the generated endpoints and Hub listener. In a container, `localhost` refers to the container itself. |
| Saved configuration changes have no effect | Apply the saved files to the stopped session, then restart its worker. |
| Conversation display looks incomplete | Run **Refresh conversation** with the worker connected. |

For container deployments and advanced settings, see the [Hub guide](hub/README.md).
The [documentation index](docs/README.md) contains the worker and Hub protocols
and configuration reference. Package README files explain individual components
for readers exploring how the harness is built.
