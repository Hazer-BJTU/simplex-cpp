# Configuration

Complete provider examples and shared field descriptions are in
[Support policy](../providers/index.md#provider-configuration),
[DeepSeek](../providers/deepseek.md#worker-configuration), and
[Qwen](../providers/qwen.md#worker-configuration).

There are three configuration layers. The Hub startup file controls listeners
and access; a launch file controls how a worker process starts; worker YAML
controls model configuration, plugins, transport, and persistence.

## Hub-managed sessions

The default data root is `~/.simplex/hub`, overridable with `--data-dir`.
The Hub seeds `configs/launch/local.jsonc`, `configs/launch/docker.jsonc`, and
`configs/worker/default.yaml` when absent. Edit these through **Configurations**
or before startup with a text editor. A session selects one launch and one
worker file and receives independent copies.

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

Role prompt YAML files live in `core/prompts/` in the source tree. Builds copy
and installs ship them under `<worker-install>/bin/prompts/`, beside the
`simplex_worker` executable. Choose a role in your worker YAML:

```yaml
worker:
  system_prompt_file: prompts/general_agent.yaml
```

This selects `<worker-install>/bin/prompts/general_agent.yaml`. The path is
relative to the running executable's directory, **not** the worker config file
or the invocation working directory. In a container, the file must exist in the
worker's installation inside that container. Omit `system_prompt_file` to use
`prompts/coding_agent.yaml`.

| File | Purpose and behavior |
| --- | --- |
| `prompts/coding_agent.yaml` | Default software-focused role with the same communication, clarification, and safety principles as the general agent. Adds repository inspection, focused code changes, compatibility and resource-lifetime considerations, relevant tests and builds, evidence-based reporting, and recovery from interruptions. |
| `prompts/general_agent.yaml` | General-purpose role for tasks beyond coding. Covers task analysis, environment checks, comparing approaches, acceptance criteria, verification, and resource cleanup. Both roles allow a relaxed, graceful, formal tone and feminine expression, require clear language and detailed clarification for unclear requirements or multiple viable approaches, and protect privacy while requiring confirmation before destructive actions. |
| `prompts/operations/compact.yaml` | An operation prompt, not a role. Loaded through `worker.compact_prompt_file` and sent as an internal user message for `compact`. Requests a structured handoff summary without tool calls, retaining useful prior memory and the user's explicit habits, preferences, and rules, including later corrections. |

Both roles help the user complete tasks and follow the user's language; their
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
signature, and any restored compaction memory separately. Keep custom role YAML
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
- Built-in tools and hooks load automatically. There is no public intrinsic
  disable/configuration mapping.

See [plugin configuration](../plugins/configuration.md) for explicit extension
selection and schema overrides.

## Paths and defaults

| Setting | Resolution or default |
| --- | --- |
| `--config` | `config.yaml` in the invocation working directory |
| Explicit plugin/configuration paths | Relative to the worker YAML's directory |
| `worker.system_prompt_file` | Relative to the executable directory; defaults to `prompts/coding_agent.yaml` |
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
