# Worker settings

Edit a worker YAML in `configs/worker/`, or pass a standalone file to
`simplex run --config`. Start a new managed session to use a different saved
configuration. The [first-session example](../getting-started/configuration.md)
shows the provider section to replace in the template.

## Managed values

| Value in worker YAML | Supplied by | Meaning |
| --- | --- | --- |
| <code v-pre>{{hub.events_endpoint}}</code> | Hub at launch | Persistent worker event and input connection (`client.endpoint`). |
| <code v-pre>{{hub.confirm_endpoint}}</code> | Hub, when confirmation is configured | One-shot approval route. |
| <code v-pre>{{hub.tools_endpoint}}</code> | Hub, when remote tools are configured | Base route for Hub tool requests. |
| <code v-pre>{{session.directory}}</code> | Hub/manager | Direct persistence directory inside the target allocation. |
| `${MODEL_API_KEY}` | Worker environment | Example credential reference in a selected provider profile. |

For a standalone worker, supply the real WebSocket URLs and a local persistence
directory. The client endpoint is required. Include optional sections only when
the corresponding service is available.

## Provider fields

Each entry under `providers` configures one model profile. Set `driver_model` to
its entry name. An optional `modality_assist_model` selects an independent
instance, even when both roles reference the same entry. In the Hub, choose the
saved worker YAML for a new managed session. Driver options selected in the
panel apply at the next run boundary.

| Worker field | Meaning |
| --- | --- |
| `plugin`, `model` | Installed plugin name (defaults to the entry name) and required service model ID. The plugin's runtime menu can be smaller than the service catalog. |
| `endpoint.base_url`, `request_path` | The path is appended to the base URL's existing prefix. Dedicated providers supply both defaults. The generic plugin needs a base URL. |
| `endpoint.auth` | Use `bearer` (default), `custom_header` or `none`. Supply `api_key` for authenticated requests. For a custom header, `header_name` defaults to `x-api-key`. |
| `endpoint.user_agent`, `extra_headers` | Override the plugin's User-Agent or add string-valued HTTP headers. Extra headers are applied last and can replace authentication headers. |
| `endpoint.extras`, `endpoint.auth.extras` | Optional endpoint metadata, separate from the request body. |
| `config` | Model generation fields. The worker supplies the conversation and tool declarations. Each provider guide describes its transformations and supported controls. |
| `retry` | `max_attempts` defaults to 3 retries after the first request. Use 0 for a single attempt. Backoff starts at `initial_backoff_ms` (500) and is capped by `max_backoff_ms` (120000). |

The worker expands `${NAME}` in API keys and extra header values when it loads
a selected profile. Set credentials in the environment of the actual worker
process, including inside Docker when applicable.

For a complete provider fragment and the plugin's runtime options, choose a
[provider guide](../providers/index.md).

## System prompts

```yaml
worker:
  system_prompt_file: prompts/default.yaml
  environment:
    workspace: ./project
    platform: Linux
    software: [git, python3]
```

All four worker prompt selectors are relative to the **worker executable's
directory**. Use nonempty relative paths without `..`. The shipped defaults are:

| Field under `worker` | Installed default | Used for |
| --- | --- | --- |
| `system_prompt_file` | `prompts/default.yaml` | Base instructions for a new session. |
| `compact_prompt_file` | `prompts/operations/compact.yaml` | Explicit compaction. |
| `auto_compact_prompt_file` | `prompts/operations/auto_compact.yaml` | Summarizing a task during automatic compaction. |
| `auto_compact_continue_prompt_file` | `prompts/operations/auto_compact_continue.yaml` | Private continuation after automatic compaction. |

For a custom prompt, install a YAML file such as `bin/prompts/my_agent.yaml`
and select `prompts/my_agent.yaml`. The **prompt file itself** can contain:

```yaml
heading_level: 2
sections:
  - name: persona
    title: Role
    stability: immutable
    text: Help the user complete the requested task.
```

Use unique section names and string text. Heading levels range from 1 to 6
(default 2). Optional stability groups appear in order: immutable (default),
growing, volatile. Leave `skill.*`, `environment.runtime`, `signature.runtime`
and `memory.runtime` to the worker. A restored session retains its saved base
prompt and memory. Runtime tool skills and environment hints are refreshed.

`environment.workspace` resolves relative to the **worker config file**.
It is a prompt hint and, when using PTC, its execution directory. Platform is a
string and software a list of strings. All three default to empty.

## Tools and approvals

```yaml
tools:
  intrinsic: [reading, editing, hub_remote_call]
security:
  confirmation:
    endpoint: '{{hub.confirm_endpoint}}'
    timeout_ms: 120000
hub_remote_call:
  endpoint: '{{hub.tools_endpoint}}'
  question_timeout_ms: 300000
```

- `tools.intrinsic` is the complete list of built-in toolsets for this worker.
  An empty list disables all built-ins; omission considers all eligible sets.
  `plugins.providers.directories` controls provider discovery. Under
  `plugins.extensions`, `tools` and `loop_hooks` have separate directory and
  enable lists. [Names, prerequisites and extension examples](../plugins/configuration.md)
- `security.confirmation.endpoint` enables external approval, with a default
  deadline of 120000 ms. Without it, an approval-required call in ask mode is denied.
- `hub_remote_call.endpoint` enables remote requests when the toolset is
  selected. `timeout_ms` defaults to 120000. `question_timeout_ms` defaults
  to 300000 and accepts 60000–1800000 ms.
- `ptc.executable` and optional string list `ptc.arguments` select the interpreter
  and modules. This requires enabled persistence, an explicit directory and
  the `ptc` toolset. [PTC configuration](../plugins/ptc.md)

## Connections and queues

For example, in a Hub worker template keep the managed event endpoint and set
a longer reconnect ceiling with this fragment:

```yaml
client:
  endpoint: '{{hub.events_endpoint}}'
  transport:
    max_backoff_ms: 20000
```

| Field | Default | Effect |
| --- | --- | --- |
| `client.payload_capacity`, `signal_capacity`, `query_capacity` | 256, 256, 64 | Separate message capacities for run inputs, control signals and read queries. |
| `client.transport.write_capacity`, `write_byte_capacity` | 256 messages, 16777216 bytes | Outgoing queue bounds. The byte limit must cover at least 16 MiB. |
| `client.transport.initial_backoff_ms`, `max_backoff_ms` | 250, 10000 ms | Reconnection delay range. The maximum must be at least the initial delay. |
| `client.transport.idle_timeout_seconds` | 0 | Zero permits idle connections indefinitely. Positive values enable an idle timeout. |
| `worker.event_capacity` | 1024 | Queue slots for ordinary display and query events. |

## Loop and compaction

```yaml
worker:
  max_exchanges: 512
  empty_advance: true
  auto_compact_threshold: 100000
  max_auto_compactions: 5
```

| Field under `worker` | Default | Effect |
| --- | --- | --- |
| `max_exchanges` | 512 | Positive limit on committed model responses per ordinary invocation. |
| `empty_advance` | true | Continue a tool-free response with empty answer content. Disable for models that cannot use an empty assistant turn. Summary requests disable it. |
| `auto_compact_threshold` | 0 (off) | With persistence enabled, set a positive threshold for the latest exchange's prompt plus generated tokens. A continuing run triggers above the threshold or at the exchange cap. Final answers take priority. |
| `max_auto_compactions` | 5 | Positive maximum number of attempts per user request. An immediate retrigger after the first resumed exchange ends the request. |

Compaction archives the original state and replaces the turns with a summary.
Cancellation stops the request at a safe boundary. [Compaction and recovery](../core/worker-protocol.md#automatic-context-compaction)

## Persistence

```yaml
persistence:
  enabled: true
  directory: '{{session.directory}}'
  backend: sqlite
  memory_retention: {max_archives: 5}
```

| Field under `persistence` | Default | Effect |
| --- | --- | --- |
| `enabled`, `directory` | true, `./data/session` standalone | Direct session root relative to the config file. The managed template uses the allocation directory. |
| `backend` | Detect existing store; SQLite for a new empty directory | Select `sqlite` or `json`. Selection does not convert existing data. |
| `state`, `memory` | `state`, `memory` | Relative child directory names or paths without parent traversal. SQLite archives live in its database; JSON archives use the memory directory. |
| `restore` | `if_present` | Load existing state. `never` starts fresh in the same destination and subsequent saves can replace previous state. |
| `memory_retention.max_archives` | 5 | Target number of retained compaction archives. Zero disables automatic cleanup. |
| `save.on_step_finished`, `on_run_finished`, `on_shutdown` | All true | Optional checkpoints after a settled step, run, or orderly shutdown. Required recovery checkpoints around tools still apply. |

Inspect and back up the complete session as described in
[Persistence backend](../architecture/persistence-backends.md#inspection-and-backups).

## Controlled research budgets

`experimental` means settings for controlled AI research, not an unstable-feature
switch. The shipped example is entirely commented out. Omitted or zero limits
disable the corresponding control:

```yaml
experimental:
  cumulative_step_control: 100
  cumulative_token_control:
    type: total
    threshold: 1000000
  model_visible_budget:      # optional
    after_exchanges: 80
    token_percent: 80
```

One step is one committed driver-model exchange, including final answers and
compact summaries. Each exchange adds its reported usage again; this is cumulative
request cost, not the size of the current context. Types are `prompt`, `generated`,
`total` (prompt + generated), and `cache_hit`. Cached tokens are included in
prompt/total without deduction or double counting. Cache-hit accounting requires
a measured cache count from the provider, including a count read through a
configured token-cost pointer. Measured zero is valid. A missing or invalid
measurement ends the run with an accounting error. Reasoning
cannot be selected separately with the current usage representation. Assistant
models and subagents are not part of this budget.

The first **strict exceedance** ends the conversation after the current tool
batch has settled. Equality does not stop execution, and a single exchange can
overshoot the limit. This budget does not predict provider context capacity or
limit the cost of incomplete/failed HTTP attempts for which no response usage is
available. Missing required usage or counter overflow ends with an explicit error.

Only committing a new external user message resets the budget. Continue, internal
continuation, auto compact and history pruning preserve it. Budget exhaustion
prevents another continue from issuing model requests; send a new message instead.
Compact costs count even if its summary later fails validation. A valid compact
replacement is published before applying its budget, without starting an automatic
continuation afterwards. Cancellation remains cancellation and does not reset counts.

To make the budget visible to the driver model, set either reminder threshold.
`after_exchanges` counts completed model replies since the last external user
message. Its maximum is `cumulative_step_control`. `token_percent` is an integer
from 1 to 100 and requires a positive `cumulative_token_control.threshold`.
Omission or zero disables the corresponding reminder trigger.

Once either threshold is reached, each ordinary request immediately following a
settled tool batch carries a short planning reminder at the end of the **last
tool result**. It tells the model how many steps it can take within the limit
and how many tokens of the configured kind remain. A token-only budget cannot
predict a step count. Each model reply consumes one step, including a reply
that requests several tools. At the exact step limit the reminder asks the model
to finish on the current reply, consistent with strict exceedance.

The reminder is attached only to the outgoing tool result. Enabled persistence
retains accounting across restarts. A changed enabled policy requires a new
user message. An exhausted budget reports a completed research stop; missing
required measurements report failure. Check `run_finished.experimental` for the
policy, counts, and reason. [Worker protocol](../core/worker-protocol.md)
