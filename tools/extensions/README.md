# Dynamic toolset extensions

This package lets a host discover a `ToolSet` in a shared module, construct it from editable YAML, and explicitly add it to a session's `ToolRegistry`. It does not change tool scheduling or the `ToolInterface` checkpoint contract. Built-in toolsets remain under `tools/intrinsic/`.

## Package layout and build

Place each module under `tools/extensions/<package>/` with its own source, schema files, and tests. Build it as a CMake `MODULE` linked to `tools_extensions` and `boost_dll_iface`. Set `LIBRARY_OUTPUT_DIRECTORY` to `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/tools`; the release installer derives `<prefix>/bin/plugins/tools` from this output path. Call `simplex_install_tool_extension_schema(<name> <absolute-schema-directory>)` to copy YAML to the build tree and install it under `<prefix>/bin/schemas/tools/extensions/<name>/`. The [no-op stub](stubs/noop/) is a complete, side-effect-free example.

The loader scans `<executable_dir>/plugins/tools` by default; `load(path)` scans a caller-selected directory. Both are nonrecursive. A nonexistent directory yields zero additions. `SIMPLEX_TOOL_EXTENSION_SCHEMA_DIR` overrides the **schema root**, so a plugin named `example` reads `<override>/example/config.yaml`. Without an override it reads `<executable_dir>/schemas/tools/extensions/example/config.yaml`. An explicit, missing override fails creation; it never falls back to source YAML. The environment must be configured before `create()`. Names must be single portable path segments (ASCII letters, digits, `_`, `-`).

## Exported contract

Include `tools/extensions/plugin.hpp`. The module provides a concrete `ToolSetExtensionContext` reporting its stable `name()` and `kAbiVersion`, and exports exactly these Boost.DLL aliases:

```cpp
std::unique_ptr<extension::ExtensionContext> create_toolset_plugin();
std::unique_ptr<tools::ToolSet> create_toolset(
    const tools::extensions::ToolSetConfig& config);

BOOST_DLL_ALIAS(my_namespace::create_toolset_plugin, create_toolset_plugin)
BOOST_DLL_ALIAS(my_namespace::create_toolset, create_toolset)
SIMPLEX_EXPORT_PLUGIN_MAGIC;
```

Put the factories in a named namespace with external linkage, as in the stub. Export the plugin magic **once** in the module. The generic loader checks its toolchain fingerprint before invoking either factory. The domain loader then checks ABI version, descriptor type, portable and unique name, and the toolset factory alias. Rejected modules are skipped. Descriptor factories must throw exceptions derived from `std::exception`, as required by the generic loader; directory enumeration failures propagate to the caller. `load()` counts newly accepted descriptors; it does not register a toolset or read its YAML.

`create(name)` loads and checks YAML, calls the factory, checks the returned set's name, and gives the host a `shared_ptr<ToolSet>`. A missing, malformed, or incompatible file or a failed factory yields `nullptr` with a diagnostic. Add the result to `ToolRegistry` only when non-null. Configure and remove registry entries at serialized session boundaries; do not mutate the registry concurrently with an executing batch.

The returned set pins its module while it lives. Its `dispatch()` and `prepare()` results also pin the underlying set and module, because a tool handle can survive removal of the registry entry. The wrapper delegates `prepare()` and `execute()` to the plugin's set so plugin overrides retain their behavior. A batch already started holds its set until its calls finish. The generic loader uses process-resident dynamic loading; do not design a plugin around unloading and reloading code during a live session.

## YAML and configuration

Each package has `schemas/config.yaml`:

```yaml
name: example
description: Explain the toolset to the host operator.
config: {}
```

All three fields are required. The common loader rejects unknown top-level keys, an invalid name, a name different from the descriptor, empty description, and a non-mapping `config`. The plugin factory validates its own options, including unknown options, before returning the set. Changing YAML affects the next constructed instance; it does not hot-reload an existing one.

For each offered tool, place `<tool-name>.yaml` in the same directory and call `tools::extensions::load_tool(config, "tool-name")` from the plugin. This reuses the intrinsic tool declaration parser and verifies that the YAML name matches the filename. The parser validates the model-facing argument schema; implementation behavior such as `InvokeType`, security and side effects still belongs in C++. An optional `skill.yaml` is loaded by `tools::extensions::load_skill(config)`, which reuses the intrinsic skill parser and returns `nullopt` only when the file is absent. Malformed present files are errors. A plugin may choose to require or omit a skill, but must not advertise a tool whose declaration or implementation is incomplete.

## Host usage and verification

```cpp
tools::extensions::ToolSetExtensionLoader loader;
loader.load_default();
if (auto set = loader.create("example")) {
    registry.add(std::move(set));
}
```

The `noop_toolset` stub publishes one `noop_probe` tool. It returns a fixed text result without external effects, so the extension tests can prove discovery, configuration, registration, routing, and invocation. The test fixtures also exercise wrong ABI, wrong context type, missing product factory, missing magic, malformed configuration, duplicate loads, and a retained tool handle after the loader and registry are cleared. Keep the stub's ABI and factory signatures in step with changes to this contract; bump the corresponding ABI version in `versioning/CMakeLists.txt` for any binary-incompatible change and rebuild host and plugins in the same toolchain context.

An explicit configuration path can be passed as the second argument to `create()`: the schema directory for toolsets. It takes precedence over discovery and environment overrides. Low-level configuration helpers throw on invalid input; instance creation catches factory and configuration exceptions and returns `nullptr`.


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
