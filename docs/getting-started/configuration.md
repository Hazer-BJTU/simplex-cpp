# Configuration

For complete provider-only YAML examples and field descriptions, see the
[provider configuration tutorial](../tutorials/provider-configuration.md).


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
