# Configure plugins

Apply the following fragments to the worker YAML on the target. Restart the
worker to load the selected components. In a Hub-managed deployment, a new
configuration pair takes effect in a new session. [Configuration library](../hub/configurations.md)

## Built-in toolsets

Choose the **complete** list under `tools.intrinsic`. For example, to let the
model run processes, read files, and edit text:

```yaml
tools:
  intrinsic: [process, reading, editing]
```

An empty list disables all built-in tools. Omitting the list selects all
built-ins whose prerequisites are met. Add `modality_assist` for image
descriptions, `hub_remote_call` for Hub plans and delegation, or
`archive_memory` for archived conversation text.
The optional modality assistant toolset is constructed only when
`modality_assist_model` is selected. The optional Hub remote toolset is constructed
only when `hub_remote_call` is configured. These receive application dependencies
during runtime construction. With persistence enabled and the selected backend
SQLite, `archive_memory` provides `memory_search` and `memory_fetch`.
For Python programs, add `ptc` and configure the
[interpreter and persistent session directory](ptc.md).

Individual tools can have host settings in their installed YAML file. For
example, edit the existing `config` section of
`bin/schemas/reading/read_text.yaml` to cap the returned text at 32 KiB:

```yaml
config:
  max_file_bytes: 1073741824
  max_output_bytes: 32768
```

The other fields in that file describe the tool. The model cannot override
these host limits with call arguments. For shell commands, the `config.bypass`
section in `bin/schemas/process/run_command.yaml` controls which commands can
bypass approval. The shipped mode is `strict`. [Process approval settings](https://github.com/Hazer-BJTU/simplex-cpp/tree/main/tools/intrinsic/toolsets/process#startup-command-bypass-policy)

## Model providers

Give a provider configuration a name and select it for a model role:

```yaml
providers:
  primary:
    plugin: deepseek
    model: deepseek-flash
    endpoint:
      auth:
        scheme: bearer
        api_key: ${MODEL_API_KEY}
driver_model: primary
```

The worker discovers all compatible provider modules. Only the named model
roles create instances. To use a vision-capable model for image descriptions,
set `modality_assist_model` to its configured name and select the
`modality_assist` toolset. Using the same name for both roles creates separate
instances. [Provider-specific settings](../providers/index.md#provider-configuration)

## Loop hooks

The worker installs the built-in `context_statistic` hook for each session.
It records context usage in the session state. Set the positive integer
`config.context_window_tokens` in `bin/schemas/loop/context_statistic/config.yaml`
to the window size used for those statistics. The model's token limit and the
worker's automatic compaction threshold are configured separately.

## Dynamic toolsets and hooks

Enable installed extensions by name. These bundled examples expose the
diagnostic tool `noop_probe` and attach a hook that leaves session state intact:

```yaml
plugins:
  extensions:
    tools:
      enable: [{name: noop_toolset}]
    loop_hooks:
      enable: [{name: noop_hook}]
```

An empty or omitted `enable` list selects no extensions of that kind. A
selected extension that cannot be found or initialized prevents worker startup.

| Module kind | Worker YAML directory list | Default beside the worker executable |
| --- | --- | --- |
| Providers | `plugins.providers.directories` | `plugins/llm/` |
| Toolsets | `plugins.extensions.tools.directories` | `plugins/tools/` |
| Hooks | `plugins.extensions.loop_hooks.directories` | `plugins/loop/` |

An empty or omitted directory list uses the default. A nonempty list replaces
it. Searches cover files directly inside each directory. Relative paths in
worker YAML resolve against the configuration file's directory.

A toolset reads its declarations from `schemas/tools/extensions/<name>/`
beside the executable. Set `schema_directory` alongside its `name` to select
another directory. For a hook, set `config_file` alongside its `name` to
override `schemas/loop/extensions/<name>/config.yaml`.
To change the default schema root for a whole deployment, set
`SIMPLEX_TOOL_EXTENSION_SCHEMA_DIR` or `SIMPLEX_LOOP_EXTENSION_SCHEMA_DIR`
in the worker environment. Explicit per-extension paths take precedence.
