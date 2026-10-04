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

## Intrinsic tool configuration

An individual built-in tool may define a host-only `config` mapping in its
existing `schemas/<tool_name>.yaml`. For example, the installed
`bin/schemas/reading/read_text.yaml` includes:

```yaml
name: read_text
# Keep the existing description and argument_schema alongside this mapping.
config:
  max_file_bytes: 16777216
  max_output_bytes: 65536
```

`read_text` accepts integer byte counts: `max_file_bytes` from 1 to 1073741824
(default 16777216), and `max_output_bytes` from 1 to 16777216 (default 65536).
These cap loaded file bytes and rendered text bytes respectively, in both line
and byte modes. Metadata and result framing are outside the display cap.
Increasing the limits permits more synchronous IO and memory use. Truncation
hints report the effective output cap; model-facing guidance does not hard-code
operator-configurable limits.

Omitting `config` means `{}`. Explicit null, scalars, and sequences are invalid.
The shared loader checks only its mapping shape and preserves nested JSON data;
the concrete tool checks supported keys, types, ranges and defaults. It does not
apply argument-schema keywords to configuration. Tools without implemented
settings accept only omitted/empty config, rather than silently ignoring keys.
Invalid configuration names the file/field in logs and makes the affected tool
unavailable; unrelated valid tools can still register.

The file is read once per instance. Newly constructed instances see edits;
existing instances keep owned settings. There are no per-call reads, hot reload,
environment substitution, or additional startup configuration fields. Use the
existing resource selection: for reading, a nonempty
`SIMPLEX_READING_SCHEMA_DIR` override, then executable-relative
`schemas/reading/`, then the compiled source directory. A selected invalid file
does not fall back to another copy. Restart a worker to construct fresh tools.

These settings belong to that **individual tool**, not its toolset or
`skill.yaml`. They never enter advertised tool schemas, invocation arguments,
injected skill text, or persisted conversation state. Model arguments cannot
replace them. YAML `type` and `security` remain documentation-only; C++ assigns
the actual concurrency/security policy.

This is distinct from a dynamic toolset's separate `config.yaml` and from a loop
hook's required `config` mapping. It also does not move application-injected model
or Hub endpoint dependencies into tool YAML. See the
[reading package guide](https://github.com/Hazer-BJTU/simplex-cpp/tree/main/tools/intrinsic/toolsets/reading)
for invocation formats and result semantics.

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
