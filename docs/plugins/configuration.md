# Configure plugins

The worker's YAML separates provider discovery from explicitly enabled tool and
hook extensions:

```yaml
plugins:
  providers:
    directories: []
  extensions:
    tools:
      directories: []
      enable:
        - name: noop_toolset
    loop_hooks:
      directories: []
      enable:
        - name: noop_hook
```

The no-op modules are diagnostic examples shipped by the source build. The toolset
exposes `noop_probe`, which returns `noop`; the hook listens without changing state.
These names demonstrate loading end to end rather than useful task behavior.

## Search locations

| Resource | Default beneath the executable directory |
| --- | --- |
| Provider modules | `plugins/llm/` |
| Tool modules | `plugins/tools/` |
| Hook modules | `plugins/loop/` |
| Tool YAML | `schemas/tools/extensions/<name>/` |
| Hook YAML | `schemas/loop/extensions/<name>/config.yaml` |

Omitted or empty directory lists use defaults. A nonempty list replaces the
default search list. Scans are nonrecursive. Explicit relative paths resolve
against the startup YAML directory. Names are portable identifiers, not paths.
Duplicate selections and unresolved selected components fail startup.

A missing directory adds no descriptors. Other filesystem failures may propagate.
Malformed modules can be logged and skipped during scanning; a selected missing
component still cannot be constructed successfully.

## Configuration overrides

```yaml
plugins:
  extensions:
    tools:
      directories: [/opt/my-plugins/tools]
      enable:
        - name: example_toolset
          schema_directory: ./plugin-config/example_toolset
    loop_hooks:
      directories: [/opt/my-plugins/loop]
      enable:
        - name: example_hook
          config_file: ./plugin-config/example_hook/config.yaml
```

Tool overrides name a **directory**; hook overrides name a **file**. With no
explicit override, `SIMPLEX_TOOL_EXTENSION_SCHEMA_DIR` or
`SIMPLEX_LOOP_EXTENSION_SCHEMA_DIR` can replace the corresponding schema root.
An explicit missing override is an error, not permission to fall back elsewhere.

Each component's `config.yaml` contains `name`, `description`, and a `config`
mapping. Identity must match the descriptor. Unknown top-level component fields
are rejected; plugin-specific options are validated by the factory. Editable YAML
changes apply to newly constructed instances, not to an already running worker.

## Built-in components and roles

Built-in process, reading, and editing toolsets and built-in hooks are assembled
by `core`. Their public enablement is not controlled by extension lists.
The optional modality assistant toolset is constructed only when
`modality_assist_model` is selected. The optional Hub remote toolset is constructed
only when `hub_remote_call` is configured. These receive application dependencies
during runtime construction.

Model selection is separate:

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

There is no provider allowlist. `primary` is a configured instance name and
`deepseek` is a factory name. Referencing the same entry for two model roles
still creates two model instances.
