# Built-in loop hooks

Built-in hooks are linked directly into the host. They do not use `extensions`
declarations or dynamic loading. `loop::LoopHookInterface` and
`loop::LoopHookRegistry` remain the common interface and session-level owner;
this package supplies the YAML configuration boundary for built-in hooks.

Put each concrete hook under `hooks/<name>/` with its own `schemas/config.yaml`,
sources and tests. The YAML has this shape:

```yaml
name: example_hook
description: >-
  Explain what this hook does to the host operator.
config:
  limit: 8
```

`name` must match the compiled package name and contain only ASCII letters,
digits, `_` or `-`. The common loader rejects unknown top-level keys, missing
fields and non-mapping `config`. Each concrete hook validates every option it
uses, including unknown options, before registration. Event subscriptions and
the meaning of each option stay in C++; changing YAML changes values when a new
hook instance is constructed, without recompilation. It does not hot-reload a
live instance or authorize arbitrary event handlers.

To construct a hook, load with `load_hook_config(hook_config_file(name), name)`.
Derive from `IntrinsicLoopHook`, validate `config().config` in the derived
constructor, then give the completed `shared_ptr` to `LoopHookRegistry::add()`
or `set()`. For a host that should continue without a broken hook,
`try_create_hook(file, name, factory)` loads, calls the concrete factory and
logs either a YAML or option-validation failure, returning `nullptr` before
any subscription is made. `try_load_hook_config` provides the same tolerant
policy for the document alone. Do not persist a hook instance: persistent
conversation and recovery data belongs in `AgentInputState`.

The lookup order for `hook_config_file(name)` is:

1. `SIMPLEX_LOOP_HOOK_SCHEMA_DIR/<name>/config.yaml`, if the environment
   variable is nonempty;
2. `<executable_dir>/schemas/loop/<name>/config.yaml`, if that directory exists;
3. this source tree's `hooks/<name>/schemas/config.yaml` during development.

A concrete package calls
`simplex_install_intrinsic_loop_hook_config(<name> <absolute YAML path>)` in
its `CMakeLists.txt`. That installs the editable YAML file to
`<prefix>/bin/schemas/loop/<name>/config.yaml` with `cmake --install`.
Listing a YAML file as a library source is useful for IDEs, but does not copy
it to the build or install tree. No concrete hook is included yet, so there is
currently no hook config file to export; the install rule is ready for each
hook package when added.
