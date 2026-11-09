# Configuration

Complete provider examples and shared field descriptions are in
[Support policy](../providers/index.md#provider-configuration),
[DeepSeek](../providers/deepseek.md#worker-configuration),
[Qwen](../providers/qwen.md#worker-configuration),
[GLM (Zhipu AI)](../providers/glm.md#worker-configuration), and
[Custom Chat Completions](../providers/chat-completions.md#minimal-worker-configuration).

There are three configuration layers. The Hub startup file controls listeners
and access; a launch file controls how a worker process starts; worker YAML
controls model configuration, plugins, transport, and persistence.

## Hub-managed sessions

The default data root is `~/.simplex/hub`, overridable with `--data-dir`.
The Hub seeds `configs/launch/local.jsonc`, `configs/launch/docker.jsonc`, and
`configs/worker/default.yaml` when absent. Edit these through **Configurations**
or before startup with a text editor. A session selects one launch and one
worker file and receives independent copies. New `local` launch files use the
same `management.executable` for `simplex run` and `simplex manage`; set it to
an absolute installed `bin/simplex` path if necessary. Their target allocations
live under `<dataDir>-workers`, independently of Hub records. The seeded
`docker` launch file is a managed target for a container the operator already
runs (`management.target.kind: "docker"`); it never creates a container. See
[Docker worker deployment](../deployment/docker-worker.md). Existing saved
launch files are preserved.

Replace `YOUR_*` values in the worker template. `providers` keys are arbitrary
configuration names; `plugin` selects an installed implementation. `driver_model`
references a configuration name, not necessarily a plugin name.

The following complete minimal worker configuration is suitable for a
Hub-managed local session using the bundled DeepSeek implementation:

```yaml
providers:
  primary:
    plugin: deepseek
    endpoint:
      base_url: https://api.deepseek.com
      request_path: /chat/completions
      auth:
        scheme: bearer
        api_key: ${MODEL_API_KEY}
    model: deepseek-flash
    config:
      reasoning_effort: high
    retry:
      max_attempts: 3
      initial_backoff_ms: 500
      max_backoff_ms: 120000

driver_model: primary
client:
  endpoint: '{{hub.events_endpoint}}'
security:
  confirmation:
    endpoint: '{{hub.confirm_endpoint}}'
    timeout_ms: 120000
worker:
  max_exchanges: 512
  environment:
    workspace: /absolute/path/to/project
persistence:
  enabled: true
  backend: sqlite
  directory: '{{session.directory}}'
  state: state
  memory: memory
  memory_retention:
    max_archives: 5
  restore: if_present
```

This example uses the current plugin's advertised model identifier. Verify that
your provider account accepts it. See [DeepSeek](../providers/deepseek.md) for
supported options and implementation scope. Retry count means retries **after**
the first attempt. It does not limit the number of agent-loop exchanges.

The Hub fills managed endpoints and the session directory at launch. Those
markers are not worker-side template variables. For a manually started worker,
replace them with actual URLs and a local persistence path first.

## System prompts

System prompt YAML files live in `core/prompts/` in the source tree. Builds copy
and installs ship them under `<worker-install>/bin/prompts/`, beside the
`simplex_worker` executable. Select the default in your worker YAML:

```yaml
worker:
  system_prompt_file: prompts/default.yaml
```

This selects `<worker-install>/bin/prompts/default.yaml`. The path is
relative to the running executable's directory, **not** the worker config file
or the invocation working directory. In a container, the file must exist in the
worker's installation inside that container. Omit `system_prompt_file` to use
`prompts/default.yaml` with current workers. Hub-generated configurations and the
bundled worker template omit this setting by default, so older deployed workers
can select their own shipped prompt. Explicit custom paths are passed unchanged.

`default.yaml` covers both general-purpose and coding tasks. Its four sections
set the role, language style, task execution, and privacy/safety guidance. It asks
for clear, respectful communication, bounded work, verification, cleanup, and
explicit confirmation where appropriate.

The former `coding_agent.yaml` and `general_agent.yaml` files are no longer shipped.
On the next start of an existing session using the built-in direct-binary
launcher, the Hub removes a saved selection of `prompts/coding_agent.yaml` or
`prompts/general_agent.yaml` only if that file is missing and the same worker
installation contains a regular `prompts/default.yaml`. It saves the omission
atomically and logs the migration once. Other settings and YAML comments remain
intact. Retained legacy files, custom paths, and installations without the new
default are not migrated. A missing custom file still fails; for a known local
installation, the Hub start error names the file and explains how to inherit the
worker default or install the selected file.

This automatic migration covers existing Hub-owned local configuration files.
It does not change immutable manager-owned configurations, SSH/Docker targets,
arbitrary command launchers, or binaries resolved only through `PATH`. Manage
those upgrades separately. For an editable configuration, remove
`worker.system_prompt_file` or select `prompts/default.yaml` after upgrading the
worker; remove any obsolete Hub-level `worker.systemPromptFile` override as well.
When rolling back to an older worker, keep the setting omitted so that worker
selects its own default. If you need a particular old prompt, install it and
restore its explicit selection before starting the worker.

Existing restored snapshots keep their saved base prompt; the new template applies
to new sessions.

The default helps the user complete tasks and follow the user's language. Its
instructions guide model behavior and do not change tool permissions or the
worker's security policy.

### Custom prompt files

Place a custom YAML file under the installed worker's `bin/prompts/` directory
and set `worker.system_prompt_file`, for example `prompts/my_agent.yaml`. Prompt
paths must be nonempty relative paths without `..` components; absolute and
rooted paths are rejected. These checks do not isolate the filesystem or prevent
symlinks from pointing elsewhere.

A minimal custom role file is:

```yaml
heading_level: 2
sections:
  - name: persona
    title: Role
    stability: immutable
    text: |
      You are a helpful general-purpose agent. Help the user complete their tasks.
      Respond clearly in the user's language.
```

Sections render in list order. Each requires a unique, nonempty `name` and a
string `text`; `title` is optional and an empty title omits its heading.
`heading_level` defaults to `2` and accepts integers from `1` to `6`. Section
`stability` defaults to `immutable`; if mixed, sections must appear in the order
`immutable`, `growing`, then `volatile`. Names beginning with `skill.` and the
names `environment.runtime`, `signature.runtime`, and `memory.runtime` are
reserved for the worker. Prompt text is not expanded from environment variables.

The worker injects active tool skills, configured environment hints, a runtime
signature, and any restored compaction memory separately. The environment section
always ends with the initial UTC system time, captured once per worker startup
(including restore), with an instruction to recheck the clock when needed. It is
not refreshed per request. Keep custom role YAML
focused on base instructions.

### Startup and restored sessions

The selected files are read and validated at every startup. Missing or invalid
files prevent startup, including when restoring state; there is no embedded
fallback or hot reload. To select them in the Hub, edit the worker configuration
in the configuration library. For an existing session, stop the worker, apply
the saved configuration, then restart.

A new session uses the selected role prompt. A restored session retains the base
prompt saved in its `AgentInputState`; changing the file or selector does not
replace that prompt. Current tool skills, environment hints, and the signature
are refreshed, while compaction memory is preserved. Create a new session to use
a different role without modifying saved state. The compact operation prompt is
loaded from the current startup configuration independently of the saved role.

## Credentials and optional components

`${NAME}` expansion is supported in instantiated provider API keys and extra
header values; it is not a general YAML templating system. Referenced environment
variables must be nonempty. Supported authentication schemes are `bearer`,
`custom_header`, and `none`.

- Omit `modality_assist_model` to leave the auxiliary model and its tool unloaded.
- Omit `hub_remote_call` to leave remote tools unloaded.
- Omit `security.confirmation` to deny confirmation-required calls in `ask`
  mode. A payload selecting `approve` can still authorize them locally.
- Select built-in toolsets with `tools.intrinsic` (see below). Intrinsic hooks
  and dynamic plugins are independent of this list.

See [plugin configuration](../plugins/configuration.md) for explicit extension
selection and schema overrides.

## Paths and defaults

| Setting | Resolution or default |
| --- | --- |
| `--config` | `config.yaml` in the invocation working directory |
| Explicit plugin/configuration paths | Relative to the worker YAML's directory |
| `worker.system_prompt_file` | Relative to the executable directory; defaults to `prompts/default.yaml` |
| `worker.compact_prompt_file` | Same rule; defaults to `prompts/operations/compact.yaml` |
| `worker.environment.workspace` | Config-relative prompt hint; does not call `chdir` or sandbox tools |
| `persistence.directory` | Direct session root, config-relative when not absolute |
| `persistence.state`, `persistence.memory` | Relative children of the persistence root; default `state` and `memory` |
| `worker.max_exchanges` | 512 model responses per ordinary invocation |

Prompt-file paths reject rooted paths and `..`; state/memory paths must also be
nonempty relative paths without parent traversal. These lexical checks do not
provide filesystem isolation.

The complete commented template is maintained in
[load/schemas/config.example.yaml](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/load/schemas/config.example.yaml).
Unknown host fields are tolerated, but malformed recognized fields are errors;
plugin configuration has its own stricter validation rules.

## Optional automatic compaction

```yaml
worker:
  max_exchanges: 512
  auto_compact_threshold: 100000 # example; choose for your provider and task
  max_auto_compactions: 5
  # auto_compact_prompt_file: prompts/operations/auto_compact.yaml
  # auto_compact_continue_prompt_file: prompts/operations/auto_compact_continue.yaml
```

Omit the threshold or use zero to disable. Positive thresholds require persistence
and trigger after settled continuing exchanges; the exchange cap is a fallback.
Final answers complete normally. Both prompts are installation-relative YAML:
the first preserves goals, important facts/retrieval references and task state;
the second resumes privately from memory. Neither replaces the role/system prompt.
The same cancel action stops the whole task, including summary inference.

Avoid tiny thresholds or exchange caps. Immediate retrigger after one continuation
exchange fails clearly, and at most five automatic compaction attempts are allowed
per request by default. A new request resets this budget. `max_auto_compactions`
is separate from `persistence.memory_retention.max_archives`. The next explicit
continue can resume from memory even when compaction left no turns; restart does
not resume automatically. See the [complete lifecycle contract](../core/worker-protocol.md#automatic-context-compaction).

Automatic compaction uses the same bounded summary preview as manual compaction.
The full summary is replaced in `<persistence.directory>/memory.md`; oversized
output is truncated for injection and display rather than causing failure.
Replacement budgeting allows 4 KiB above the nominal context/reduction target;
transmitted summaries still fit 32 KiB including their pointer and marker.
An actual compact failure ends the request immediately; the attempt limit is
not a failed-summary retry loop.

## Persistence backend

New empty sessions use SQLite (`state/state.sqlite3`), including compact archives.
This requires a worker/manager build with SQLite support; older workers should
continue using their saved JSON configuration.
Existing JSON sessions remain supported: keep `backend: json` or the legacy
`format: json` setting. Saved Hub configurations remain authoritative at restart.
Omitting both settings detects an existing single backend; a directory holding
both stores is refused for every setting. Changing a backend setting is **not
migration**, and selecting the opposite backend over existing history is rejected. See
[state and persistence](../architecture/state-and-persistence.md#selection-inspection-and-backup)
for inspection, backup, and child-session behavior.

## Intrinsic toolset selection

```yaml
tools:
  intrinsic: [process, reading, editing, archive_memory, modality_assist, hub_remote_call]
```

The list is an explicit allowlist: omitted toolsets are never registered, even
when their configuration exists. Registration follows list order; duplicates
are removed. Unknown names and malformed lists fail startup. An empty list
disables all intrinsic tools. Omitting the list preserves legacy automatic
loading, including PTC when configured.

Selected conditional toolsets are skipped with a startup diagnostic unless their
dependencies exist: `archive_memory` requires the selected SQLite backend,
`modality_assist` requires an auxiliary model, `hub_remote_call` requires its
endpoint, and `ptc` requires interpreter configuration. Invalid supplied
configuration still fails validation, including PTC without enabled persistence
and an explicit directory. Selection does not disable provider model roles or
change how their configuration is validated.

For PTC-based file/process operations, use
`[ptc, archive_memory, modality_assist, hub_remote_call]` and configure the
[interpreter](../plugins/ptc.md). Native tools are not removed implicitly.
Only registered tools contribute their skills. Loop hooks and explicitly enabled
dynamic toolsets are unaffected.

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
```

One step is one committed driver-model exchange, including final answers and
compact summaries. Each exchange adds its reported usage again; this is cumulative
request cost, not the size of the current context. Types are `prompt`, `generated`,
`total` (prompt + generated), and `cache_hit`. Cached tokens are included in
prompt/total without deduction or double counting. Cache-hit accounting requires
an explicit reported cache count in the preserved standard usage metadata;
missing/unsupported usage is an accounting error, not measured zero. Reasoning
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

Counters and terminal reasons live in the host-owned `simplex.experimental` state
metadata. Enabled persistence retains them across restarts; without persistence
they last only for this worker process. Enabling or changing an enabled research policy requires a
new user message, rather than reinterpreting old measurements. With controls
enabled, settled accounting is saved even if ordinary `save_run` is disabled.
`run_finished.experimental` reports policy, steps, selected tokens, reason and
accounting error. Budget exhaustion uses terminal status `completed`; actual
accounting failures use `failed`. This takes precedence over per-run
`max_exchanges` / `auto_compact_threshold` continuation behavior.
