# Dynamic loop hook extensions

This package loads `LoopHookInterface` implementations from shared modules. The host constructs a hook from editable YAML and explicitly adds it to its session's `LoopHookRegistry`. Dynamic and intrinsic hooks share the same synchronous event bus, event semantics, subscription order, state-edit validation, and recovery rules in [`../README.md`](../README.md). This package does not introduce asynchronous hook callbacks.

## Package layout and build

Give each module a package directory under `loop/extensions/`, with source, `schemas/config.yaml`, and tests. Build a CMake `MODULE` linked to `loop_extensions` and `boost_dll_iface`, with `LIBRARY_OUTPUT_DIRECTORY` set to `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/loop`. The release installer then places it at `<prefix>/bin/plugins/loop`. Call `simplex_install_loop_extension_config(<name> <absolute-config-file>)` to copy and install its YAML at `<prefix>/bin/schemas/loop/extensions/<name>/config.yaml`. The [no-op stub](stubs/noop/) shows the full shape.

`load_default()` scans `<executable_dir>/plugins/loop`; `load(path)` scans an explicit directory. Scans are nonrecursive. A nonexistent directory yields zero additions. `config_file(name)` selects `<executable_dir>/schemas/loop/extensions/<name>/config.yaml`, or `<SIMPLEX_LOOP_EXTENSION_SCHEMA_DIR>/<name>/config.yaml` when the override is set. An explicit, missing override does not silently fall back to source files. Configure it before calling `create()`. Plugin names may contain only ASCII letters, digits, `_`, or `-` and cannot be paths.

## Exported contract

Include `loop/extensions/plugin.hpp`. Implement `LoopHookExtensionContext` and export two Boost.DLL aliases with the exact signatures below:

```cpp
std::unique_ptr<extension::ExtensionContext> create_loop_hook_plugin();
std::unique_ptr<loop::LoopHookInterface> create_loop_hook(
    const loop::intrinsic::HookConfig& config);

BOOST_DLL_ALIAS(my_namespace::create_loop_hook_plugin, create_loop_hook_plugin)
BOOST_DLL_ALIAS(my_namespace::create_loop_hook, create_loop_hook)
SIMPLEX_EXPORT_PLUGIN_MAGIC;
```

Use externally linked functions in a named namespace, and emit the magic block once per module. The generic loader checks the build fingerprint before invoking the exported factories. The loop loader checks `kAbiVersion`, descriptor type, portable and unique name, and the hook factory alias. It skips rejected modules while continuing to scan others. Descriptor factories must throw exceptions derived from `std::exception`, as required by the generic loader; directory enumeration failures propagate to the caller. `load()` counts newly accepted descriptors but neither parses YAML nor attaches callbacks.

`create(name)` reuses the intrinsic `HookConfig` parser, requires the YAML name to match the descriptor, calls the plugin factory, and verifies the returned hook's name. The factory validates every plugin-specific option before returning; malformed config or a failed factory yields `nullptr` with a diagnostic. The result's deleter pins its dynamic library through destruction, even after the loader is gone.

## YAML, registration, and lifetime

A configuration file has the same contract as an intrinsic hook:

```yaml
name: example_hook
description: Explain this hook to the host operator.
config: {}
```

The common parser rejects unknown top-level keys, missing fields, invalid types and names, and mismatched identity. `config` must be a mapping; `{}` means no options. YAML changes apply to the next constructed instance only. Persistent state belongs in `AgentInputState`; a hook object may keep process-local state across runs.

```cpp
loop::extensions::LoopHookExtensionLoader loader;
loader.load_default();
if (auto hook = loader.create("example_hook")) {
    hook_registry.add(std::move(hook));
}
```

`LoopHookRegistry` attaches callbacks to the same bus passed to `loop::run()`. Its binding disconnects subscriptions before releasing the hook; the hook's custom deleter keeps the module loaded until the most-derived destructor finishes. The bus must outlive the registry. Add, replace, or remove hooks only at a boundary serialized with `run()` and event publication, never while one of their callbacks is executing. A plugin's `subscribe()` must adopt each connection into `ScopedSubscription` before inserting it into a vector, so allocation failure cannot leave a dangling callback. See `LoopHookInterface` for the exact rule.

The `noop_hook` stub subscribes to `RunStarted` without changing state. Tests load the real module, construct and register it, publish an event, remove it, and check bad ABI, wrong context type, missing factory, missing magic, malformed configuration, and duplicate loads. Bump `SIMPLEX_LOOP_HOOK_PLUGIN_ABI_VERSION` in `versioning/CMakeLists.txt` on binary-incompatible contract changes; build host and plugins in the same toolchain context.

An explicit configuration path can be passed as the second argument to `create()`: the YAML file for hooks. It takes precedence over discovery and environment overrides. Low-level configuration helpers throw on invalid input; instance creation catches factory and configuration exceptions and returns `nullptr`.


## Ownership and compatibility rules

The configuration reference passed to a product factory is valid only for that
call. Copy the fields needed by the returned object into its own storage;
never retain references into the temporary configuration. Factories must return
fully initialized objects and clean up partial construction through RAII.
Destructors must not throw.

The ABI version covers all types crossing this boundary, including inherited
interfaces, configuration, model I/O data, and (for hooks) events. A compatible
compiler fingerprint alone does not make a changed C++ layout compatible. Rebuild
plugins after changing the contract and update the domain ABI constant when
required. Plugins are trusted native code; admission checks are compatibility
checks, not a sandbox.

The default install layout is:

```text
<prefix>/
  bin/
    <host executable>
    plugins/
      tools/libtools_extension_noop.so
      loop/libloop_extension_noop.so
    schemas/
      tools/extensions/noop_toolset/
        config.yaml
        noop_probe.yaml
        skill.yaml
      loop/extensions/noop_hook/config.yaml
  lib/
    libextension_tools.so
    libextension_loop_hooks.so
    <shared dependencies>
```

Executable-relative discovery also works after relocating the complete install
tree. The staged-runtime CI job copies the integration test executables into
this layout and exercises the installed modules and YAML, separately from tests
in the build tree.
